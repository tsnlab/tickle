#!/usr/bin/env python3
"""kh2_ab_read.py - the reading rules of kh2_ab_pc.sh, written 2026-10-24 before any of its runs (ROADMAP "Other open
items", RESULTS KH2).

The question: the wait ring's first move at 2 waits instead of 64 (ab/kh2-receive-return) - does the pong's executor
now receive by the ring from its first measured round trip, is that round trip's receive faster for it, and is nothing
after it slower? On the PC: two private netns over veth, rmw_wake_split.sh runs (ping --wait block, Bench
BEST_EFFORT), both nodes under sysstamp (the libc builds, -DTT_HAL_LIBC_CALLS).

Arms (one build pair, kh2_pc_build.sh, parent and branch, the same flags):
  P   parent, the ring on (default)              B   branch, the ring on
  PC  parent, TT_RX_URING_WAIT=0 (no ring)       BC  branch, TT_RX_URING_WAIT=0 - THE CONTROL: without a ring the move
                                                     rule never runs, so PC and BC can differ only by the PC's drift.
Per run, per measured round trip (ping seq >= 1, in order), on the pong:
  path       how the pong's reply thread received the ping: "ring" (an io_uring_enter, ret >= 0, the last one on that
             thread to return between the ping's send call and the reply's send call: the wait ring's record carries no
             payload, as rmw_pcap_split.py's ring_receive() says) or "epoll" (a recv* call carrying the ping's send_ns).
  kfwd       the ping's send call entered -> the pong's receive returned (sysstamp's CLOCK_REALTIME on one host): kernel
             transmit, veth, the pong's wake and its receive call(s) - KH2's "tap -> receive return" plus the ping's
             kernel transmit, which no arm changes.
  first      kfwd median over round trips 1-20 (where P's executor is still on the epoll path, as on the rig);
  steady     kfwd median over round trips 41 and on;  rtt  median round trip (ping stamps), all round trips.
Rules, in this order:
  VOID run   its status is not "ok" (rmw_wake_split.sh: library identity, RESULT line); the 1-minute load average read
             just before it was above 4.0 (other sessions' work: the PC is shared); fewer than 200 round trips; fewer
             than 90% of its round trips with a path and kfwd.
  VOID       fewer than 3 valid runs in any arm.
  TREATMENT  (not realized -> VOID, the run did not test the change): every valid P run has >= 5 epoll round trips among
             1-20; every valid B run has <= 1 epoll round trip in all; every PC and BC run has no ring round trip.
  CONTROL    PC's and BC's per-run "first" ranges must overlap, and their "steady" ranges too; apart -> VOID (the PC
             moved between arms by more than the change could show).
  PRIMARY    "first": IMPROVED when every valid B run is below every valid P run (range rule); WORSE when every B run is
             above every P run; otherwise NOT RESOLVED.
  GUARD      "steady" and "rtt": WORSE when every B run is above every P run (the ring moving early must cost nothing
             once it has moved); reported as held otherwise.
  Verdict    VOID; else WORSE if the primary or a guard is WORSE; else the primary's word.
Exit 0 a verdict (IMPROVED, NOT RESOLVED), 1 WORSE, 3 VOID.

Usage: kh2_ab_read.py <out dir of kh2_ab_pc.sh>      kh2_ab_read.py --selftest
"""
import glob
import os
import statistics
import struct
import sys
import tempfile

LOAD_MAX = 4.0
MIN_RTS = 200
MIN_JOINED = 0.9
MIN_RUNS = 3
FIRST_N = 20
STEADY_FROM = 41
P_EPOLL_MIN = 5
B_EPOLL_MAX = 1
ARMS = ("P", "B", "PC", "BC")


def read_stamps(path):
    rows, offset = {}, None
    with open(path, encoding="utf-8", errors="replace") as f:
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
    recs = []
    for path in glob.glob(f"{prefix}.sys.{role}.*"):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("#"):
                    continue
                x = line.split()
                if len(x) < 6:
                    continue
                recs.append((x[0], x[1], int(x[2]), int(x[3]), int(x[4]), x[6] if len(x) > 6 else ""))
    recs.sort(key=lambda r: r[3])
    return recs


def first_with_key(recs, prefix, key_hex, after):
    for r in recs:
        if r[3] >= after and r[1].startswith(prefix) and r[4] > 0 and key_hex in r[5]:
            return r
    return None


def run_metrics(prefix):
    """Per measured round trip, in seq order: (path, kfwd_ns or None), and the round trips' rtts."""
    ping, ping_off = read_stamps(f"{prefix}.ping.stamps")
    pong, pong_off = read_stamps(f"{prefix}.pong.stamps")
    ping_sys, pong_sys = read_sys(prefix, "ping"), read_sys(prefix, "pong")
    rtts, trips = [], []
    if ping_off is None or pong_off is None:
        return rtts, trips
    for seq in sorted(ping):
        send, reply = ping[seq]
        if seq == 0 or reply == 0 or seq not in pong:
            continue
        rtts.append(reply - send)
        kh = struct.pack("<Q", send).hex()
        rs = send + ping_off
        tx = first_with_key(ping_sys, "send", kh, rs)
        reply_tx = first_with_key(pong_sys, "send", kh, rs)
        if tx is None or reply_tx is None:
            trips.append((None, None))
            continue
        rx = first_with_key(pong_sys, "recv", kh, tx[2])
        if rx is not None and rx[3] <= reply_tx[2]:
            trips.append(("epoll", rx[3] - tx[2]))
            continue
        ring = None
        for r in pong_sys:
            if r[3] > reply_tx[2]:
                break
            if r[1] == "io_uring_enter" and r[4] >= 0 and r[0] == reply_tx[0] and r[3] >= tx[2]:
                ring = r
        trips.append(("ring", ring[3] - tx[2]) if ring else (None, None))
    return rtts, trips


def median(xs):
    return statistics.median(xs) if xs else None


def summarize(prefix):
    rtts, trips = run_metrics(prefix)
    joined = [t for t in trips if t[0] is not None]
    first = [t[1] for t in trips[:FIRST_N] if t[0] is not None]
    steady = [t[1] for t in trips[STEADY_FROM - 1:] if t[0] is not None]
    return {
        "n": len(rtts), "joined": len(joined),
        "epoll_first": sum(1 for t in trips[:FIRST_N] if t[0] == "epoll"),
        "epoll": sum(1 for t in joined if t[0] == "epoll"), "ring": sum(1 for t in joined if t[0] == "ring"),
        "first": median(first), "steady": median(steady), "rtt": median(rtts),
    }


def apart_below(lo, hi):
    return bool(lo) and bool(hi) and max(lo) < min(hi)


def read(runs):
    """runs: [(arm, rep, load1, status, summary dict)]. Returns (verdict, lines)."""
    lines, valid = [], {a: [] for a in ARMS}
    for arm, rep, load, status, s in runs:
        why = []
        if status != "ok":
            why.append(f"status {status}")
        if load > LOAD_MAX:
            why.append(f"load {load:.2f} > {LOAD_MAX}")
        if s["n"] < MIN_RTS:
            why.append(f"{s['n']} round trips < {MIN_RTS}")
        elif s["joined"] < MIN_JOINED * s["n"]:
            why.append(f"{s['joined']} of {s['n']} round trips joined")
        us = {k: (f"{s[k] / 1000:.2f}" if s[k] is not None else "na") for k in ("first", "steady", "rtt")}
        lines.append(f"  {arm:2s} rep{rep} load {load:.2f} n={s['n']} joined={s['joined']} ring={s['ring']} "
                     f"epoll={s['epoll']} (first {FIRST_N}: {s['epoll_first']}) first={us['first']} "
                     f"steady={us['steady']} rtt={us['rtt']} us" + (f"  VOID: {'; '.join(why)}" if why else ""))
        if not why and s["first"] is not None and s["steady"] is not None:
            valid[arm].append(s)
    for arm in ARMS:
        if len(valid[arm]) < MIN_RUNS:
            return "VOID", lines + [f"VOID: arm {arm} has {len(valid[arm])} valid runs < {MIN_RUNS}"]
    bad = []
    if any(s["epoll_first"] < P_EPOLL_MIN for s in valid["P"]):
        bad.append(f"a P run has < {P_EPOLL_MIN} epoll round trips among the first {FIRST_N} (the PC run did not "
                   "reach the case the change is for)")
    if any(s["epoll"] > B_EPOLL_MAX for s in valid["B"]):
        bad.append(f"a B run has > {B_EPOLL_MAX} epoll round trips (the change did not act)")
    if any(s["ring"] > 0 for s in valid["PC"] + valid["BC"]):
        bad.append("a control run received by a ring (TT_RX_URING_WAIT=0 not applied)")
    if bad:
        return "VOID", lines + ["TREATMENT NOT REALIZED -> VOID: " + "; ".join(bad)]
    lines.append("TREATMENT realized: P on epoll at the start, B on the ring throughout, controls without a ring")
    for metric in ("first", "steady"):
        pc, bc = [s[metric] for s in valid["PC"]], [s[metric] for s in valid["BC"]]
        if apart_below(pc, bc) or apart_below(bc, pc):
            return "VOID", lines + [f"CONTROL FAIL ({metric}): PC {min(pc) / 1000:.2f}-{max(pc) / 1000:.2f} and BC "
                                    f"{min(bc) / 1000:.2f}-{max(bc) / 1000:.2f} us apart -> VOID"]
        lines.append(f"CONTROL PASS ({metric}): PC {min(pc) / 1000:.2f}-{max(pc) / 1000:.2f}, BC "
                     f"{min(bc) / 1000:.2f}-{max(bc) / 1000:.2f} us overlap")

    def word(metric):
        p, b = [s[metric] for s in valid["P"]], [s[metric] for s in valid["B"]]
        w = "IMPROVED" if apart_below(b, p) else ("WORSE" if apart_below(p, b) else "NOT RESOLVED")
        lines.append(f"{metric:6s} P {min(p) / 1000:.2f}-{max(p) / 1000:.2f} (median {median(p) / 1000:.2f}), B "
                     f"{min(b) / 1000:.2f}-{max(b) / 1000:.2f} (median {median(b) / 1000:.2f}) us -> {w}")
        return w

    primary = word("first")
    guards = [word("steady"), word("rtt")]
    if primary == "WORSE" or "WORSE" in guards:
        return "WORSE", lines
    return primary, lines


def load_runs(out):
    runs = []
    with open(os.path.join(out, "manifest.txt"), encoding="utf-8") as f:
        for line in f:
            x = line.split()
            if len(x) >= 5 and x[0] == "RUNMETA":
                arm, rep, load, status, prefix = x[1], int(x[2]), float(x[3]), x[4], x[5]
                runs.append((arm, rep, load, status, summarize(prefix)))
    return runs


def selftest():
    def s(first, steady, rtt, epoll_first, epoll, ring, n=400):
        return {"n": n, "joined": n, "epoll_first": epoll_first, "epoll": epoll, "ring": ring,
                "first": first, "steady": steady, "rtt": rtt}

    def runs(p_first, b_first, pc_first=(20000, 20100, 20200), bc_first=(20050, 20150, 20250), p_epoll=15, b_epoll=0,
             b_steady=(19000, 19100, 19200), load=0.5):
        out = []
        for i in range(3):
            out.append(("P", i, load, "ok", s(p_first[i], 19050 + i, 50000, p_epoll, p_epoll, 400 - p_epoll)))
            out.append(("B", i, load, "ok", s(b_first[i], b_steady[i], 50000, b_epoll, b_epoll, 400 - b_epoll)))
            out.append(("PC", i, load, "ok", s(pc_first[i], 21000 + i, 51000, 20, 400, 0)))
            out.append(("BC", i, load, "ok", s(bc_first[i], 21001 + i, 51000, 20, 400, 0)))
        return out

    cases = [
        ("improved", runs((21000, 21100, 21200), (19000, 19100, 19200)), "IMPROVED"),
        ("overlap", runs((19000, 21100, 21200), (19500, 19100, 19200)), "NOT RESOLVED"),
        ("steady worse", runs((21000, 21100, 21200), (19000, 19100, 19200), b_steady=(30000, 30100, 30200)), "WORSE"),
        ("loaded", runs((21000, 21100, 21200), (19000, 19100, 19200), load=9.0), "VOID"),
        ("treatment: P never on epoll", runs((21000, 21100, 21200), (19000, 19100, 19200), p_epoll=0), "VOID"),
        ("treatment: B on epoll", runs((21000, 21100, 21200), (19000, 19100, 19200), b_epoll=10), "VOID"),
        ("control apart", runs((21000, 21100, 21200), (19000, 19100, 19200), bc_first=(25000, 25100, 25200)), "VOID"),
    ]
    ok = True
    for name, rs, want in cases:
        got, ls = read(rs)
        print(f"selftest {name}: {got} (want {want})")
        if got != want:
            ok = False
            print("\n".join(ls))
    # the ring/epoll classification on a tiny synthetic run
    d = tempfile.mkdtemp(prefix="kh2_ab_selftest_")
    p = os.path.join(d, "x")
    key = struct.pack("<Q", 1000).hex()
    with open(p + ".ping.stamps", "w", encoding="utf-8") as f:
        f.write("# realtime_minus_monotonic_ns_start 0\n1 1000 9000\n")
    with open(p + ".pong.stamps", "w", encoding="utf-8") as f:
        f.write("# realtime_minus_monotonic_ns_start 0\n1 5000 6000\n")
    with open(p + ".sys.ping.1", "w", encoding="utf-8") as f:
        f.write(f"7 sendto 1100 1200 16 16 {key}\n")
    with open(p + ".sys.pong.2", "w", encoding="utf-8") as f:
        f.write("9 io_uring_enter 100 3000 1 0\n9 io_uring_enter 2900 2950 -1 0\n"
                f"9 sendto 5500 5600 16 16 {key}\n")
    _, trips = run_metrics(p)
    if trips != [("ring", 3000 - 1100)]:
        ok = False
        print(f"selftest classify ring: {trips}")
    with open(p + ".sys.pong.2", "w", encoding="utf-8") as f:
        f.write(f"9 epoll_pwait2 100 2500 1 0\n9 recvfrom 2600 2700 16 16 {key}\n9 sendto 5500 5600 16 16 {key}\n")
    _, trips = run_metrics(p)
    if trips != [("epoll", 2700 - 1100)]:
        ok = False
        print(f"selftest classify epoll: {trips}")
    print("SELFTEST OK" if ok else "SELFTEST FAILED")
    return 0 if ok else 1


def main():
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(selftest())
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    verdict, lines = read(load_runs(sys.argv[1]))
    print("\n".join(lines))
    print(f"KH2 PC A/B VERDICT: {verdict}")
    sys.exit({"IMPROVED": 0, "NOT RESOLVED": 0, "WORSE": 1}.get(verdict, 3))


if __name__ == "__main__":
    main()
