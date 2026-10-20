#!/usr/bin/env python3
"""Splits rmw_wake_split.sh runs: where a round trip's receive time goes, for rmw_tickle and the vendors alike.

ROADMAP "Other open items" (2026-10-20): "rmw kernel wake (~6 us) and poll-to-executor handoff (~21 us)". Both
figures are from the rig in September (RMW_PERF_PLAN 8.1/8.2, `git show 6234e915^:rmw_tickle/RMW_PERF_PLAN.md`):
the pong's tap -> receive-return was 18.2 us against CycloneDDS's 12.6 (ppoll, then recvfrom), and its receive
-> callback 21.1 us (the poll thread's delivery, the hand-off to the executor, the take). This reads the same
two quantities on the PC, on current code, for all three rmws.

Inputs per run prefix P (rmw_wake_split.sh):
  P.ping.stamps  seq send_ns reply_ns          (CLOCK_MONOTONIC)
  P.pong.stamps  seq callback_ns publish_ret_ns
  P.sys.ping.<pid>, P.sys.pong.<pid>  optional, sysstamp.c (LD_PRELOAD, CLOCK_REALTIME): every socket and wait call
  P.pong.trace, P.ping.trace           optional, an rmw_tickle -DRMW_TICKLE_TRACE=ON dump

Segments, per round trip (the stamps files alone - any rmw):
  fwd   ping send_ns -> pong callback_ns;  turn  callback -> publish return;  back  publish return -> ping reply_ns
With sysstamp (the datagram is found by the Bench sample's send_ns, 8 bytes little-endian, in the call's head):
  ping_tx_user   send_ns -> the ping's send call entered
  kernel_fwd     the ping's send call entered -> the pong's receive call returned with that datagram. On veth the
                 receive softirq runs in the sender's call; this is kernel transmit + receive + the pong's wake + its
                 receive call(s): the "kernel wake" segment, framework-neutral.
  pong_wait_to_recv  the pong's wait call (epoll/poll family) on the receiving thread returned -> its receive call
                 returned: the second system call, 0-free for an rmw that blocks in the receive itself (none found).
  pong_rx_user   receive call returned -> callback: the rmw's receive, any thread hand-off, the executor, the take.
  pong_rx_thread_same  share of round trips whose datagram was received on the callback's thread (the pong's
                 callback thread is the one that sends the reply, so: the receive thread == the reply's sender).
  kernel_back, ping_rx_user  the same on the way back.

How to read it (written before the first full run, 2026-10-20):
- The kernel wake gap = rmw_tickle's kernel_fwd minus the vendor's, in medians, with n >= 500 round trips per arm
  and the A/A spread of rmw_tickle against itself (two runs) as the yardstick. A gap inside the A/A spread is "not
  resolved on the PC", not "zero".
- The second system call is pong_wait_to_recv (rmw_tickle only; the vendors' receive call is the wait).
- The hand-off: pong_rx_thread_same near 1.0 for rmw_tickle means no poll-thread -> executor hand-off is left on
  the path at that gap; then pong_rx_user is rmw + rclcpp work alone, to be compared with the vendors', whose
  receive thread is never the executor (expected pong_rx_thread_same 0).

Seen on the PC (2026-10-20, d25c12a2, rmw_wake_campaign.sh, 3 reps x 5 s at 5 ms and 3 x 30 s at 100 ms, Bench,
both QoS; rmw_tickle built with -DTT_HAL_LIBC_CALLS for sysstamp, lyrical's CycloneDDS and Fast DDS (UDPv4 only);
~2,900 round trips per 5 ms cell, pooled p10 / median in us):
- kernel_fwd, BEST_EFFORT: rmw_tickle 16.74 / 20.42, Fast DDS 16.77 / 18.81, CycloneDDS 17.28 / 22.21; RELIABLE
  17.49 / 23.41, 16.44 / 21.43, 17.24 / 19.46. The per-run medians' ranges overlap in every cell: no kernel wake gap
  resolved on the PC. rmw_tickle's second call (epoll_pwait2, then recvfrom) is 1.19-1.20 us at p10; lyrical's
  CycloneDDS also waits first (epoll_wait, then recvmsg: 1.32-1.33); Fast DDS blocks in recvfrom.
- pong_rx_user: rmw_tickle 3.05 / 3.30 (the datagram received on the callback's thread in every round trip),
  CycloneDDS 16.61 / 21.95, Fast DDS 23.05 / 28.92 (never the same thread). The ~21 us hand-off is gone: executor-
  driven receive (RMW_PERF_PLAN 8.4, on since 2026-09-27) removed it; the vendors pay theirs.
- At the 100 ms gap every rmw's kernel_fwd is ~110-130 us (a CPU idle for 100 ms; per-run medians), and
  pong_rx_user is rmw_tickle 14.9, CycloneDDS 126, Fast DDS 147. rmw_tickle's ping hands the role over once per
  round trip there (executor_handovers = round trips), before the reply arrives.

Usage: rmw_wake_split.py <prefix or directory> ...
       rmw_wake_split.py --pool <prefix or directory> ...   every cell's round trips pooled over its repetitions   One block per prefix; a final TSV line per prefix for tables.
"""

import glob
import os
import re
import statistics
import struct
import sys

WAITS = {"poll", "ppoll", "epoll_wait", "epoll_pwait", "epoll_pwait2", "select", "pselect"}
SENDS = {"sendto", "sendmsg", "sendmmsg"}
RECVS = {"recvfrom", "recvmsg", "recvmmsg"}
WINDOW_NS = 5_000_000  # a datagram's calls are searched this far around its round trip
P90 = 0.9


def read_stamps(path):
    rows, offset = {}, None
    with open(path) as f:
        for line in f:
            if line.startswith("# realtime_minus_monotonic_ns_start"):
                offset = int(line.split()[-1])
            if line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) == 3:
                rows[int(fields[0])] = (int(fields[1]), int(fields[2]))
    return rows, offset


def read_sys(prefix, role):
    files = glob.glob(f"{prefix}.sys.{role}.*")
    recs = []
    for path in files:
        with open(path) as f:
            for line in f:
                if line.startswith("#"):
                    continue
                fields = line.split()
                if len(fields) < 6:
                    continue
                head = bytes.fromhex(fields[6]) if len(fields) > 6 else b""
                recs.append((int(fields[2]), int(fields[3]), int(fields[0]), fields[1], int(fields[4]), head))
    recs.sort(key=lambda r: r[1])  # by return: a blocking receive is entered long before its datagram comes
    return recs


def summary(values):
    if not values:
        return "n=0"
    values = sorted(values)
    return (f"n={len(values)} median={statistics.median(values) / 1000:.2f} "
            f"p90={values[int(len(values) * P90)] / 1000:.2f}")


def median_us(values):
    return f"{statistics.median(values) / 1000:.2f}" if values else "na"


class SysIndex:
    """A process's sysstamp records, searchable by the datagram's send_ns."""

    def __init__(self, recs):
        self.recs = recs
        self.t_out = [r[1] for r in recs]

    def find(self, key, calls, after_ns):
        import bisect
        start = bisect.bisect_left(self.t_out, after_ns - WINDOW_NS)
        for i in range(start, len(self.recs)):
            rec = self.recs[i]
            if rec[1] > after_ns + WINDOW_NS:
                break
            if rec[3] in calls and rec[4] > 0 and key in rec[5]:
                return i
        return None

    def wait_before(self, index):
        """The last wait call on the receive call's own thread that returned before the receive call was entered."""
        rec = self.recs[index]
        for j in range(index - 1, max(-1, index - 400), -1):
            other = self.recs[j]
            if other[2] == rec[2] and other[3] in WAITS and other[1] <= rec[0]:
                return j
            if other[2] == rec[2] and other[3] in RECVS | SENDS:
                return None  # a receive or send came between: this receive was not preceded by its own wait
        return None


def trace_split(path):
    """rmw_tickle's own stamps: per deliver (3), is the next exec_wake (5) on the same thread, and how far away."""
    stamps = []
    try:
        with open(path) as f:
            for line in f:
                fields = line.split()
                if fields and fields[0] == "stamp":
                    stamps.append((int(fields[1]), int(fields[2]), int(fields[3])))
    except OSError:
        return None
    out = {"same": 0, "other": 0, "wake_to_dgram": [], "dgram_to_exec": []}
    for i, (ns, thread, point) in enumerate(stamps):
        if point != 3:
            continue
        dgram = wake = None
        for j in range(i - 1, max(-1, i - 40), -1):
            if stamps[j][1] != thread:
                continue
            if stamps[j][2] == 2 and dgram is None:
                dgram = stamps[j][0]
            elif stamps[j][2] == 1 and dgram is not None:
                wake = stamps[j][0]
                break
            elif stamps[j][2] == 3:
                break
        for j in range(i + 1, min(len(stamps), i + 40)):
            if stamps[j][2] == 5:
                if stamps[j][1] == thread:
                    out["same"] += 1
                else:
                    out["other"] += 1
                if dgram is not None and stamps[j][0] - dgram < WINDOW_NS:
                    out["dgram_to_exec"].append(stamps[j][0] - dgram)
                break
        if wake is not None and dgram is not None and dgram - wake < WINDOW_NS:
            out["wake_to_dgram"].append(dgram - wake)
    return out


def split(prefix):
    ping, ping_off = read_stamps(f"{prefix}.ping.stamps")
    pong, pong_off = read_stamps(f"{prefix}.pong.stamps")
    seg = {k: [] for k in ("rtt", "fwd", "turn", "back", "ping_tx_user", "kernel_fwd", "pong_wait_to_recv",
                           "pong_rx_user", "pong_tx_user", "kernel_back", "ping_wait_to_recv", "ping_rx_user",
                           "kernel_fwd_to_wait")}
    same = other = 0
    ping_sys, pong_sys = SysIndex(read_sys(prefix, "ping")), SysIndex(read_sys(prefix, "pong"))
    have_sys = bool(ping_sys.recs) and bool(pong_sys.recs)
    for seq, (send, reply) in ping.items():
        if seq == 0 or reply == 0 or seq not in pong:
            continue
        callback, published = pong[seq]
        if not send <= callback <= published <= reply:
            continue
        seg["rtt"].append(reply - send)
        seg["fwd"].append(callback - send)
        seg["turn"].append(published - callback)
        seg["back"].append(reply - published)
        if not have_sys:
            continue
        key = struct.pack("<Q", send)
        rs, rc, rp, rr = send + ping_off, callback + pong_off, published + pong_off, reply + ping_off
        tx = ping_sys.find(key, SENDS, rs)
        rx = pong_sys.find(key, RECVS, rs)
        tx2 = pong_sys.find(key, SENDS, rc)
        rx2 = ping_sys.find(key, RECVS, rp)
        if None in (tx, rx, tx2, rx2):
            continue
        t_tx, t_rx = ping_sys.recs[tx][0], pong_sys.recs[rx][1]
        t_tx2, t_rx2 = pong_sys.recs[tx2][0], ping_sys.recs[rx2][1]
        if not rs <= t_tx <= t_rx <= rc <= t_tx2 <= t_rx2 <= rr:
            continue
        seg["ping_tx_user"].append(t_tx - rs)
        seg["kernel_fwd"].append(t_rx - t_tx)
        seg["pong_rx_user"].append(rc - t_rx)
        seg["pong_tx_user"].append(t_tx2 - rc)
        seg["kernel_back"].append(t_rx2 - t_tx2)
        seg["ping_rx_user"].append(rr - t_rx2)
        if pong_sys.recs[rx][2] == pong_sys.recs[tx2][2]:
            same += 1
        else:
            other += 1
        w = pong_sys.wait_before(rx)
        if w is not None:
            seg["pong_wait_to_recv"].append(t_rx - pong_sys.recs[w][1])
            seg["kernel_fwd_to_wait"].append(pong_sys.recs[w][1] - t_tx)
        w2 = ping_sys.wait_before(rx2)
        if w2 is not None:
            seg["ping_wait_to_recv"].append(t_rx2 - ping_sys.recs[w2][1])
    print(f"== {prefix}")
    for name, values in seg.items():
        if values:
            print(f"  {name:<18} {summary(values)}")
    if have_sys:
        total = same + other
        print(f"  pong_rx_thread_same {same}/{total}")
    for role in ("pong", "ping"):
        t = trace_split(f"{prefix}.{role}.trace")
        if t is not None and (t["same"] + t["other"]) > 0:
            print(f"  trace {role}: deliver->exec_wake same thread {t['same']}/{t['same'] + t['other']}; "
                  f"rx_wake->rx_datagram {summary(t['wake_to_dgram'])}; rx_datagram->exec_wake "
                  f"{summary(t['dgram_to_exec'])}")
    cols = ["rtt", "fwd", "turn", "back", "kernel_fwd", "pong_wait_to_recv", "pong_rx_user", "kernel_back",
            "ping_rx_user"]
    print("TSV\t" + prefix + "\t" + "\t".join(f"{c}={median_us(seg[c])}" for c in cols) + f"\tn={len(seg['rtt'])}"
          + f"\tnsys={len(seg['kernel_fwd'])}")
    return seg


POOL_COLS = ["rtt", "kernel_fwd", "kernel_fwd_to_wait", "pong_wait_to_recv", "pong_rx_user", "kernel_back",
             "ping_rx_user"]


def pooled(prefixes):
    """Every round trip of a cell's repetitions pooled (the cell: the prefix without its _r<rep>): p10, p25, median.
    The low quantiles are the path's cost with the least of the PC's other load in it."""
    cells = {}
    for p in prefixes:
        cell = re.sub(r"_r\d+$", "", os.path.basename(p))
        seg = split(p)
        for k, v in seg.items():
            cells.setdefault(cell, {}).setdefault(k, []).extend(v)
    print("POOLED cell\tsegment\tn\tp10\tp25\tmedian (us)")
    for cell in sorted(cells):
        for k in POOL_COLS:
            v = sorted(cells[cell].get(k, []))
            if v:
                print(f"POOLED {cell}\t{k}\t{len(v)}\t{v[len(v) // 10] / 1000:.2f}\t{v[len(v) // 4] / 1000:.2f}"
                      f"\t{statistics.median(v) / 1000:.2f}")


def prefixes_of(arg):
    """A run prefix as given, or every run in a directory (its *.ping.stamps), sorted."""
    if os.path.isdir(arg):
        return sorted(path[: -len(".ping.stamps")] for path in glob.glob(os.path.join(arg, "*.ping.stamps")))
    return [arg]


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--pool":
        pooled([p for a in sys.argv[2:] for p in prefixes_of(a)])
        sys.exit(0)
    for a in sys.argv[1:]:
        for p in prefixes_of(a):
            split(p)
