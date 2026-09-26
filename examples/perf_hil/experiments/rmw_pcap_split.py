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


WAITS = ("poll", "ppoll", "epoll_wait", "epoll_pwait", "select", "pselect")


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
        prr = first_call(ping_r, kh, "recv", back[k])
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
    for m in re.finditer(r"^(\S+) (\S+) (\S+) rep(\d+)(?: wait=(\w+))?( sst=on)?[^|\n]*\| (\S+) \|.*?rtt_avg_ms=([\d.]+)",
                         text, re.M):
        stem = (f"{m.group(1)}_{m.group(2)}_{m.group(3)}_rep{m.group(4)}" + (f"_{m.group(5)}" if m.group(5) else "")
                + ("_sst" if m.group(6) else ""))
        rtts[stem] = (m.group(7), float(m.group(8)))
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
        print(f"   ping_send  {stats(send)}")
        print(f"   pong_turn  {stats(turn)}")
        print(f"   wire       {stats(wire)}")
        recv = rtt * 1e6 - statistics.fmean(tap_rtt)
        print(f"   ping_recv  mean {recv / 1000:7.1f} us   (app rtt_avg minus send_ns-to-echo-at-tap)")
        if bad:
            print(f"   WARNING: {bad} negative segments - offsets or matching are wrong for this row")
        matches = kernel_split(stem, full, sent, back, arr, dep, coff, rtt, out.with_suffix(out.suffix + ".sysstamp"))
        app_split(stem, full, sent, back, arr, dep, matches, out.with_suffix(out.suffix + ".stamps"))


if __name__ == "__main__":
    main()
