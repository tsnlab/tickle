#!/usr/bin/env python3
"""Split an rmw ping-pong round trip into ping send, wire, pong turnaround and ping receive, from
the pcaps rmw_crosshost_rtt.sh CAPTURE=1 records on both Pis (RMW_PERF_PLAN.md section 6).

Usage: rmw_pcap_split.py <OUT file of rmw_crosshost_rtt.sh>   (pcaps are read from <OUT>.pcaps/)

Matching needs no knowledge of any wire format. The ping's Bench sample starts with send_ns, the
ping's CLOCK_MONOTONIC at publish, and the pong echoes the sample back unchanged. On the ping host a
client->server packet is a ping when it carries, at any offset, 8 little-endian bytes within 50 ms of
(pcap time - client REALTIME-MONOTONIC offset). Those 8 bytes are then the key for the same sample in
every other packet, on both hosts and in both directions, so RTPS and TickLE are matched alike. A
retransmission repeats its key; only the first copy per host and direction counts.

Per sample, with P/R the ping host's departure/arrival and A/D the pong host's arrival/departure
(tcpdump's tap: after the driver on receive, before it on send):
  ping_send   P - (send_ns + client offset)     ping process: publish to the tap, one clock
  pong_turn   D - A                             pong host: tap to tap, kernel receive, wake, rmw, echo
  wire        (R - P) - (D - A)                 both links, NICs and drivers below the tap
  ping_recv   rtt_avg_ms - mean(R - send_ns')   mean only: the ping reports no per-sample RTT

HOW TO READ IT (written before the first capture, 2026-09-26). rmw_tickle's RTT is ~0.08 ms above
CycloneDDS's, and ~240 us of each round trip is outside rmw_tickle's stamps (RMW_PERF_PLAN.md 5).
  - CONTROL: wire must agree across the three rmw implementations within ~10 us, since it is the
    same link carrying packets of nearly the same size. If it does not, the split is measuring
    something other than what is labelled, and no other column is to be read.
  - If rmw_tickle's pong_turn exceeds CycloneDDS's by most of the deficit (>= 60 us), the deficit
    is in the pong process; RMW_PERF_PLAN's next step is inside that span (rx_wake vs tap).
  - If ping_send + ping_recv carry it instead, it is on the ping side, which the rmw trace never
    stamped, and the pong-side hypotheses (H1-H3) were refuted for that reason.
  - If it spreads with no segment above ~30 us, no single fix is indicated; report the split as is.
"""
import hashlib
import re
import statistics
import struct
import sys
from pathlib import Path

WINDOW_NS = 50_000_000
MIN_MONO_NS = 10**9


def packets(path):
    """Yields (time_ns, src_ip, dst_ip, udp_payload) for every IPv4/UDP packet of a classic pcap."""
    with open(path, "rb") as f:
        head = f.read(24)
        magic = struct.unpack("<I", head[:4])[0]
        if magic == 0xA1B23C4D:
            scale = 1
        elif magic == 0xA1B2C3D4:
            scale = 1000
        else:
            sys.exit(f"{path}: not a little-endian classic pcap")
        linktype = struct.unpack("<I", head[20:24])[0]
        if linktype != 1:
            sys.exit(f"{path}: linktype {linktype}, expected Ethernet")
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            sec, frac, incl, _ = struct.unpack("<IIII", rec)
            buf = f.read(incl)
            if struct.unpack(">H", buf[12:14])[0] != 0x0800:
                continue
            ip = buf[14:]
            ihl = (ip[0] & 0x0F) * 4
            if ip[9] != 17:
                continue
            yield sec * 10**9 + frac * scale, ip[12:16], ip[16:20], ip[ihl + 8:]


def first_time(pcap):
    return next((t for t, _, _, _ in packets(pcap)), None)


def ping_keys(pcap, offset_ns):
    """Finds the ping samples on the ping host: returns ({key: (P, send_ns)}, client_ip). The ping's
    destination is not the server's address when the rmw broadcasts (rmw_tickle does, before it has
    unicast peers), so the server is found from the echoes instead (server_ip)."""
    found = {}
    client = None
    for t, src, dst, pay in packets(pcap):
        mono = t - offset_ns
        for k in range(0, len(pay) - 7):
            v = struct.unpack_from("<Q", pay, k)[0]
            if MIN_MONO_NS < v <= mono and mono - v < WINDOW_NS:
                key = pay[k:k + 8]
                if key not in found:
                    found[key] = (t, v)
                client = client or src
                break
    return found, client


def server_ip(pcap, keys, client):
    """The source of the first packet that carries a ping's key and is not from the client."""
    for _, src, _, pay in packets(pcap):
        if src != client and any(key in pay for key in keys):
            return src
    return None


def dest_kinds(pcap, keys, src_ip):
    """How the packets from src_ip that carry a key were addressed: {'broadcast': n, 'unicast': n}."""
    kinds = {"broadcast": 0, "unicast": 0}
    for _, src, dst, pay in packets(pcap):
        if src == src_ip and any(key in pay for key in keys):
            kinds["broadcast" if dst[3] == 255 else "unicast"] += 1
    return kinds


def first_by_key(pcap, keys, src_ip):
    """{key: first time} for packets from src_ip carrying a key."""
    out = {}
    for t, src, _, pay in packets(pcap):
        if src != src_ip:
            continue
        for key in keys:
            if key not in out and key in pay:
                out[key] = t
                break
    return out


# epoll_pwait2 since 2026-10-21: rmw_tickle's wait set (hal_linux.c rx_wait()) in a TT_HAL_LIBC_CALLS build, which
# sysstamp.c records since 9d87885c; without it every rmw_tickle "tap->wake" read n/a.
WAITS = ("poll", "ppoll", "epoll_wait", "epoll_pwait", "epoll_pwait2", "select", "pselect")
# Per ok row, for wake_summary(): stem -> {"rmw", "cell", "sst", "rtt", "n", "wire", segment p50s in ns}
ROWS = {}


def sysstamp_records(directory):
    """Every record of every process dumped into <directory> by experiments/sysstamp, as tuples
    (tid, call, t_in, t_out, ret, head_hex), sorted by return time."""
    recs = []
    for path in sorted(Path(directory).glob("*")):
        for line in path.read_text().splitlines():
            if line.startswith("#"):
                continue
            f = line.split()
            recs.append((f[0], f[1], int(f[2]), int(f[3]), int(f[4]), f[6] if len(f) > 6 else ""))
    recs.sort(key=lambda r: r[3])
    return recs


def first_call(recs, key_hex, prefix, not_before):
    """The first send* or recv* record carrying the key that returned at or after not_before (so a node's own
    broadcast looping back to it, or a retransmission's second copy, is not mistaken for the packet at the
    tap), or None."""
    for r in recs:
        if r[1].startswith(prefix) and r[4] > 0 and r[3] >= not_before and key_hex in r[5]:
            return r
    return None


# io_uring_enter since 2026-10-22: TickLE's wait ring (hal_linux.c wr_wait()), which waits and reads in that one call. Its
# datagram lands in a buffer sysstamp does not see, so the record has no payload to match by key: ring_receive() takes,
# on the thread that sends the reply, the last io_uring_enter that returned between the tap and the reply's send.
RING = "io_uring_enter"


def ring_receive(recs, tap, before, tid=None):
    """The wait ring's receive: the last io_uring_enter (successful) on thread `tid` (any, if None) that returned at or
    after the tap and before `before`, or None. With one sample in flight - a ping-pong - it is the call that returned
    the datagram the tap saw."""
    best = None
    for r in recs:
        if r[1] == RING and r[4] >= 0 and tap <= r[3] <= before and (tid is None or r[0] == tid):
            best = r
    return best


def ring_next(recs, tap):
    """The first successful io_uring_enter that returned at or after the tap (the ping's receive of the echo)."""
    for r in recs:
        if r[1] == RING and r[4] >= 0 and r[3] >= tap:
            return r
    return None


def wake_before(recs, recv, tap):
    """The wait call (poll, ppoll, epoll_*, select) on the receiving thread that returned after the packet reached
    the tap and before the receive began: the thread's wake-up. None when the thread blocks in the receive
    call itself, as CycloneDDS's data thread does."""
    best = None
    for r in recs:
        if r[0] == recv[0] and r[1] in WAITS and tap <= r[3] <= recv[2]:
            best = r
    return best


def print_split(label, xs):
    if xs:
        print(f"   {label:26s} {stats(xs)}  (n={len(xs)})")
    else:
        print(f"   {label:26s} n/a")


def load_stamps(path):
    """A --stamps file (fdba1d22): {seq: (first_ns, second_ns)} on CLOCK_MONOTONIC, and the file's own
    REALTIME - MONOTONIC offset, the mean of the values read at its start and end."""
    rows, offs = {}, []
    for line in path.read_text().splitlines():
        if line.startswith("# realtime_minus_monotonic_ns_"):
            offs.append(int(line.split()[-1]))
        elif line and not line.startswith("#"):
            seq, first, second = (int(x) for x in line.split())
            rows[seq] = (first, second)
    return rows, (sum(offs) // len(offs) if offs else None)


def app_split(stem, keys, sent, back, arr, dep, matches, stamps_dir):
    """The application ends of each path, from the ping's and pong's own per-sample stamps (--stamps): when the
    reply reached the ping's callback, and when the ping reached the pong's callback and its publish returned.
    Joined with the pcaps always, and with sysstamp's records when the row has them."""
    ping_f, pong_f = stamps_dir / f"{stem}_ping.txt", stamps_dir / f"{stem}_pong.txt"
    if not ping_f.exists() or not pong_f.exists():
        print("   stamps: none for this row")
        return
    ping_rows, ping_off = load_stamps(ping_f)
    pong_rows, pong_off = load_stamps(pong_f)
    if ping_off is None or pong_off is None:
        print("   stamps: VOID, a file has no clock offset")
        return
    seq_of = {struct.pack("<Q", send): seq for seq, (send, _) in ping_rows.items()}
    seg = {k: [] for k in ("ping tap->app (reply)", "pong tap->callback", "pong callback->tap",
                           "pong recv->callback", "pong callback->send", "pong send->publish ret",
                           "ping recv->app (reply)")}
    for k in keys:
        seq = seq_of.get(k)
        if seq is None or seq not in pong_rows or ping_rows[seq][1] == 0:
            continue
        reply = ping_rows[seq][1] + ping_off
        callback, pub_ret = (x + pong_off for x in pong_rows[seq])
        seg["ping tap->app (reply)"].append(reply - back[k])
        seg["pong tap->callback"].append(callback - arr[k])
        seg["pong callback->tap"].append(dep[k] - callback)
        m = matches.get(k)
        if m:
            _, pr, pss, prr = m
            seg["pong recv->callback"].append(callback - pr[3])
            seg["pong callback->send"].append(pss[2] - callback)
            seg["pong send->publish ret"].append(pub_ret - pss[3])
            seg["ping recv->app (reply)"].append(reply - prr[3])
    print(f"   stamps: {len(seg['pong tap->callback'])} of {len(keys)} samples joined")
    for label, xs in seg.items():
        print_split(label, xs)


def kernel_split(stem, keys, sent, back, arr, dep, coff, rtt, sst_dir):
    """RMW_PERF_PLAN.md section 8: each side of the round trip split at the kernel boundary. Returns the
    matched records per key, for app_split()."""
    ping_dir, pong_dir = sst_dir / f"{stem}_ping", sst_dir / f"{stem}_pong"
    matches = {}
    if not ping_dir.is_dir() or not pong_dir.is_dir():
        print("   sysstamp: none for this row")
        return matches
    ping_r, pong_r = sysstamp_records(ping_dir), sysstamp_records(pong_dir)
    seg = {k: [] for k in ("ping app->send", "ping send->tap", "pong tap->wake", "pong tap->recv",
                           "pong recv->send (user)", "pong send->tap", "ping tap->wake", "ping tap->recv")}
    to_recv = []
    handoff = 0
    for k in keys:
        kh = k.hex()
        send_ns_rt = sent[k][1] + coff
        ps = first_call(ping_r, kh, "send", send_ns_rt)
        pr = first_call(pong_r, kh, "recv", arr[k])
        pss = first_call(pong_r, kh, "send", pr[3]) if pr else None
        if pr is None:  # a wait ring's receive: no payload in its record (RING, above)
            pss = first_call(pong_r, kh, "send", arr[k])
            pr = ring_receive(pong_r, arr[k], pss[2], pss[0]) if pss else None
        prr = first_call(ping_r, kh, "recv", back[k]) or ring_next(ping_r, back[k])
        if not (ps and pr and pss and prr):
            continue
        matches[k] = (ps, pr, pss, prr)
        seg["ping app->send"].append(ps[2] - send_ns_rt)
        seg["ping send->tap"].append(sent[k][0] - ps[2])
        w = wake_before(pong_r, pr, arr[k])
        if w:
            seg["pong tap->wake"].append(w[3] - arr[k])
        seg["pong tap->recv"].append(pr[3] - arr[k])
        seg["pong recv->send (user)"].append(pss[2] - pr[3])
        handoff += pss[0] != pr[0]
        seg["pong send->tap"].append(dep[k] - pss[2])
        w = wake_before(ping_r, prr, back[k])
        if w:
            seg["ping tap->wake"].append(w[3] - back[k])
        seg["ping tap->recv"].append(prr[3] - back[k])
        to_recv.append(prr[3] - send_ns_rt)
    n = len(seg["ping app->send"])
    row = ROWS.get(stem)
    if row is not None:
        row["n"] = n
        for label in ("pong tap->wake", "pong tap->recv", "ping tap->recv"):
            if seg[label]:
                row[label] = sorted(seg[label])[len(seg[label]) // 2]
    print(f"   sysstamp: {n} of {len(keys)} samples found in both hosts' records "
          f"(pong receive and reply on different threads in {handoff})")
    for label, xs in seg.items():
        print_split(label, xs)
    if to_recv:
        print(f"   {'ping recv->app (mean)':26s} {rtt * 1e3 - statistics.fmean(to_recv) / 1000:7.1f} us"
              "   (app rtt_avg minus send_ns-to-receive-return)")
    return matches


def stats(xs):
    xs = sorted(xs)
    return (f"mean {statistics.fmean(xs) / 1000:7.1f}  p50 {xs[len(xs) // 2] / 1000:7.1f}  "
            f"p90 {xs[int(len(xs) * 0.9)] / 1000:7.1f} us")


def main():
    out = Path(sys.argv[1])
    text = out.read_text()
    offsets = {}
    for m in re.finditer(r"clock_offset_ns stem=(\S+) before: client=(-?\d+) server=(-?\d+) "
                         r"after: client=(-?\d+) server=(-?\d+)", text):
        c0, s0, c1, s1 = (int(x) for x in m.groups()[1:])
        offsets[m.group(1)] = ((c0 + c1) // 2, abs(c1 - c0), abs(s1 - s0))
    # Freshness (2026-09-26): the first CAPTURE session copied one stale file into every row, and matching
    # by key could not tell, because REALTIME - MONOTONIC barely moves between runs. So each capture has to
    # start within seconds of its own run's recorded t0, and no two rows may share a pcap.
    t0 = {(m.group(1), m.group(2)): int(m.group(3))
          for m in re.finditer(r"capture stem=(\S+) role=(\S+) t0_ns=(\d+)", text)}
    seen = {}
    rtts = {}
    # A row is "<rmw> <msg> <qos> rep<N>[ wait=<mode>][ other tags] | <verdict> | ...", and its pcaps' stem
    # is <rmw>_<msg>_<qos>_rep<N>[_<mode>] (rmw_crosshost_rtt.sh WAITS).
    pat = (r"^(\S+) (\S+) (\S+) rep(\d+)(?: wait=(\w+))?(?: poll_sleep_us=(\d+))?( sst=on)?[^|\n]*\| (\S+) \|"
           r".*?rtt_avg_ms=([\d.]+)")
    for m in re.finditer(pat, text, re.M):
        stem = (f"{m.group(1)}_{m.group(2)}_{m.group(3)}_rep{m.group(4)}" + (f"_{m.group(5)}" if m.group(5) else "")
                + (f"_ps{m.group(6)}" if m.group(6) else "") + ("_sst" if m.group(7) else ""))
        rtts[stem] = (m.group(8), float(m.group(9)))
        if m.group(8) == "ok":
            ROWS[stem] = {"rmw": m.group(1), "cell": f"{m.group(2)} {m.group(3)}", "sst": bool(m.group(7)),
                          "rtt": float(m.group(9))}
    for stem, (coff, cdrift, sdrift) in offsets.items():
        ping, pong = out.with_suffix(out.suffix + ".pcaps") / f"{stem}_ping.pcap", \
            out.with_suffix(out.suffix + ".pcaps") / f"{stem}_pong.pcap"
        verdict, rtt = rtts.get(stem, ("missing", float("nan")))
        print(f"== {stem}  (row {verdict}, app rtt_avg {rtt:.3f} ms, offset drift client {cdrift} ns "
              f"server {sdrift} ns)")
        if not ping.exists() or not pong.exists():
            print("   VOID: pcap missing")
            continue
        stale = []
        for role, pcap in (("ping", ping), ("pong", pong)):
            digest = hashlib.md5(pcap.read_bytes()).hexdigest()
            if digest in seen:
                stale.append(f"{role} pcap identical to {seen[digest]}'s")
            seen[digest] = stem
            start, first = t0.get((stem, role)), first_time(pcap)
            if start is None:
                stale.append(f"no capture t0 for {role}")
            elif first is None or not start - 2 * 10**9 <= first <= start + 5 * 10**9:
                stale.append(f"{role} pcap does not start at this run's t0")
        if stale:
            print(f"   VOID: {'; '.join(stale)}")
            continue
        sent, cli = ping_keys(ping, coff)
        if not sent:
            print("   VOID: no ping sample found in the ping host's capture")
            continue
        srv = server_ip(ping, sent, cli)
        if srv is None:
            print("   VOID: no echo found in the ping host's capture")
            continue
        print(f"   ping packets {dest_kinds(ping, sent, cli)}, echo packets {dest_kinds(ping, sent, srv)} "
              f"(every copy, retransmissions included)")
        back = first_by_key(ping, sent, srv)
        arr = first_by_key(pong, sent, cli)
        dep = first_by_key(pong, sent, srv)
        full = [k for k in sent if k in back and k in arr and k in dep]
        print(f"   matched {len(full)} of {len(sent)} pings (echo seen {len(back)}, "
              f"pong host in {len(arr)} out {len(dep)})")
        if not full:
            print("   VOID: nothing matched on both hosts")
            continue
        send = [sent[k][0] - sent[k][1] - coff for k in full]
        turn = [dep[k] - arr[k] for k in full]
        wire = [(back[k] - sent[k][0]) - (dep[k] - arr[k]) for k in full]
        tap_rtt = [back[k] - sent[k][1] - coff for k in full]
        bad = sum(1 for x in send + turn + wire if x < 0)
        if stem in ROWS:
            ROWS[stem]["wire"] = sorted(wire)[len(wire) // 2]
        print(f"   ping_send  {stats(send)}")
        print(f"   pong_turn  {stats(turn)}")
        print(f"   wire       {stats(wire)}")
        recv = rtt * 1e6 - statistics.fmean(tap_rtt)
        print(f"   ping_recv  mean {recv / 1000:7.1f} us   (app rtt_avg minus send_ns-to-echo-at-tap)")
        if bad:
            print(f"   WARNING: {bad} negative segments - offsets or matching are wrong for this row")
        matches = kernel_split(stem, full, sent, back, arr, dep, coff, rtt, out.with_suffix(out.suffix + ".sysstamp"))
        app_split(stem, full, sent, back, arr, dep, matches, out.with_suffix(out.suffix + ".stamps"))
    wake_summary()
    ring_summary()


def range_rule(subj, other):
    """SLOWER when every subject rep is above every other rep, FASTER when every one is below, else NOT RESOLVED."""
    if min(subj) > max(other):
        return "SLOWER"
    if max(subj) < min(other):
        return "FASTER"
    return "NOT RESOLVED"


def wake_summary():
    """The Pi's kernel wake split after the hold-over (58bd6b1a), the ROADMAP item "rmw kernel wake (~6 us)": in
    September the pong's tap -> receive return read 18.2 us for rmw_tickle against CycloneDDS's 12.6.

    HOW TO READ IT, written 2026-10-21 before the run and enforced here:
      K0 SAMPLES FIRST. Only rows the harness marked ok are read; a row's sysstamp segments only with >= 50 samples
         matched on both hosts (a default rmw_tickle build makes raw system calls sysstamp cannot see, so its sst rows
         are expected NOT READ - the check that the libc variant is what makes them visible). An rmw with fewer than
         2 readable reps in a cell is NOT READ there.
      K1 CONTROL per cell: the wire (both links below the tap) is the same link for every rmw, so the reps' median
         wire p50s of the readable rmws must agree within 10 us; otherwise the cell's K2 verdicts are NOT READ.
      K2 THE KERNEL WAKE, per cell, rmw_tickle (the TT_HAL_LIBC_CALLS variant when present) against each vendor:
         per-rep p50 of "pong tap->recv" by the range rule (SLOWER / FASTER / NOT RESOLVED), the median difference
         beside it, and the same for "pong tap->wake" (the wait's return; a vendor blocking in its receive has none).
         NOT RESOLVED means "not resolved on the rig at this n", not "zero".
      K3 the instrument's cost: per rmw and cell, median RTT with sysstamp minus without (diagnostic, not a verdict).
    """
    if not ROWS:
        return
    print("\n=== kernel wake summary (K0-K3) ===")
    for cell in sorted({r["cell"] for r in ROWS.values()}):
        on = [r for r in ROWS.values() if r["cell"] == cell and r["sst"]]
        off = [r for r in ROWS.values() if r["cell"] == cell and not r["sst"]]
        rmws = sorted({r["rmw"] for r in on + off})
        print(f"-- {cell}")
        for rmw in rmws:
            a = [r["rtt"] for r in on if r["rmw"] == rmw]
            b = [r["rtt"] for r in off if r["rmw"] == rmw]
            if a and b:
                print(f"   K3 {rmw:24s} rtt sst-on - sst-off "
                      f"{(statistics.median(a) - statistics.median(b)) * 1e3:+7.1f} us (reps {len(a)}/{len(b)})")
        read = {}
        for rmw in rmws:
            rr = [r for r in on if r["rmw"] == rmw and r.get("n", 0) >= 50 and "pong tap->recv" in r and "wire" in r]
            if len(rr) < 2:
                nn = [r.get("n", 0) for r in on if r["rmw"] == rmw]
                print(f"   K0 {rmw:24s} NOT READ: {len(rr)} readable sst reps (matched samples per rep {nn})")
                continue
            read[rmw] = rr
            print(f"   K0 {rmw:24s} {len(rr)} reps, matched samples {[r['n'] for r in rr]}")
        if len(read) < 2:
            print("   K1 NOT READ: fewer than two rmws readable")
            continue
        wires = {rmw: statistics.median(r["wire"] for r in rr) for rmw, rr in read.items()}
        spread = (max(wires.values()) - min(wires.values())) / 1000
        ok = spread <= 10.0
        print(f"   K1 wire p50 per rmw {', '.join(f'{k} {v / 1000:.1f}' for k, v in wires.items())} us: spread"
              f" {spread:.1f} us -> CONTROL {'PASS' if ok else 'FAIL'}")
        subj = [k for k in read if k.startswith("rmw_tickle@libc")] or [k for k in read if k.startswith("rmw_tickle")]
        if not subj:
            print("   K2 NOT READ: no rmw_tickle arm readable")
            continue
        s = subj[0]
        for v in [k for k in read if not k.startswith("rmw_tickle")]:
            for seg in ("pong tap->recv", "pong tap->wake"):
                xs = [r[seg] / 1000 for r in read[s] if seg in r]
                ys = [r[seg] / 1000 for r in read[v] if seg in r]
                if len(xs) < 2 or len(ys) < 2:
                    print(f"   K2 {s} vs {v} {seg}: n/a ({len(xs)}/{len(ys)} reps have it)")
                    continue
                verdict = range_rule(xs, ys) if ok else "NOT READ (K1)"
                print(f"   K2 {s} vs {v} {seg}: {statistics.median(xs):.1f} vs {statistics.median(ys):.1f} us"
                      f" (diff {statistics.median(xs) - statistics.median(ys):+.1f}) -> {verdict}")



def ring_summary():
    """The wait ring's rig A/B (rig_queue_rx_uring_wait.sh, written 2026-10-22 before the run and enforced here): the
    variants rmw_tickle@libc and @default carry the ring (the branch's tt_RX_URING_WAIT 1), @libc_ringoff and @ringoff
    are built with -Dtt_RX_URING_WAIT=0 (the epoll path). Printed only when a ringoff variant is in the rows.
      RING0 SAMPLES FIRST: an arm is read in a cell only with >= 3 ok reps; a sysstamp rep only with >= 50 matched
            samples and its "pong tap->recv".
      RING1 CONTROL: the libc arms' median wire p50s within 10 us, else RING2 is NOT READ.
      RING2 THE RECEIVE RETURN, per-rep p50 of "pong tap->recv", libc against libc_ringoff: IMPROVED when the range rule
            says FASTER and the median of the reps is at least 0.5 us lower; otherwise NOT IMPROVED (NOT READ when
            RING0/RING1 say so).
      RING3 THE ROUND TRIP without the instrument (sst off), default against ringoff, per-rep rtt_avg: by the range rule;
            PASS unless SLOWER.
      RING_VERDICT PASS iff RING2 IMPROVED and RING3 PASS (stage R); RING_RTT_VERDICT is RING3 alone (stage M, where
            nothing is captured).
    """
    if not any(r["rmw"].endswith("ringoff") for r in ROWS.values()):
        return
    print("\n=== wait ring summary (RING0-RING3) ===")
    ring2, ring3 = "NOT READ", "NOT READ"
    for cell in sorted({r["cell"] for r in ROWS.values()}):
        print(f"-- {cell}")
        rows = [r for r in ROWS.values() if r["cell"] == cell]

        def sst(rmw):
            return [r for r in rows if r["sst"] and r["rmw"] == rmw and r.get("n", 0) >= 50 and "pong tap->recv" in r]

        on, off = sst("rmw_tickle@libc"), sst("rmw_tickle@libc_ringoff")
        if on or off:
            if len(on) < 3 or len(off) < 3:
                print(f"   RING0 NOT READ: readable sst reps libc {len(on)}, libc_ringoff {len(off)} (3 needed)")
            elif any("wire" not in r for r in on + off):
                print("   RING1 NOT READ: a rep without its wire p50")
            else:
                w_on = statistics.median(r["wire"] for r in on) / 1000
                w_off = statistics.median(r["wire"] for r in off) / 1000
                ok = abs(w_on - w_off) <= 10.0
                print(f"   RING1 wire p50 libc {w_on:.1f} libc_ringoff {w_off:.1f} us -> CONTROL {'PASS' if ok else 'FAIL'}")
                if ok:
                    xs = [r["pong tap->recv"] / 1000 for r in on]
                    ys = [r["pong tap->recv"] / 1000 for r in off]
                    rule = range_rule(xs, ys)
                    diff = statistics.median(xs) - statistics.median(ys)
                    ring2 = "IMPROVED" if rule == "FASTER" and diff <= -0.5 else "NOT IMPROVED"
                    print(f"   RING2 pong tap->recv p50 per rep libc {[round(x, 2) for x in xs]} libc_ringoff "
                          f"{[round(y, 2) for y in ys]} us: {rule}, median {diff:+.2f} us -> {ring2}")
        plain = {v: [r["rtt"] * 1e3 for r in rows if not r["sst"] and r["rmw"] == f"rmw_tickle@{v}"]
                 for v in ("default", "ringoff")}
        if len(plain["default"]) < 3 or len(plain["ringoff"]) < 3:
            print(f"   RING0 NOT READ (RING3): rtt reps default {len(plain['default'])}, ringoff {len(plain['ringoff'])}")
        else:
            rule = range_rule(plain["default"], plain["ringoff"])
            diff = statistics.median(plain["default"]) - statistics.median(plain["ringoff"])
            ring3 = "FAIL" if rule == "SLOWER" else "PASS"
            print(f"   RING3 rtt_avg per rep default {[round(x, 1) for x in plain['default']]} ringoff "
                  f"{[round(y, 1) for y in plain['ringoff']]} us: {rule}, median {diff:+.1f} us -> {ring3}")
    print(f"RING_VERDICT {'PASS' if ring2 == 'IMPROVED' and ring3 == 'PASS' else 'FAIL'} (RING2 {ring2}, RING3 {ring3})")
    print(f"RING_RTT_VERDICT {ring3 if ring3 != 'NOT READ' else 'NOT_READ'}")


if __name__ == "__main__":
    main()
