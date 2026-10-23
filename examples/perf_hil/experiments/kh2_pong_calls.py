#!/usr/bin/env python3
"""kh2_pong_calls.py - what the pong's reply thread did between a ping reaching the tap and the reply's send, per
sample, from a KH/KH2 run's own files (rmw_crosshost_rtt.sh CAPTURE=1 SYSSTAMP_ARMS=on; read with rmw_pcap_split.py's
own matching, nothing re-implemented). Diagnostic for ROADMAP "Other open items" / RESULTS KH2 (2026-10-24), not a
verdict: it says which system calls lie inside "pong tap->recv" for each rmw.

Per sample, on the thread that sent the reply:
  asleep    the receive call (rmw_pcap_split.py's choice: the recv* carrying the key, or for the wait ring the last
            io_uring_enter before the send) was entered before the tap - the thread was asleep in it when the ping
            arrived, so tap -> its return is kernel time only (no user code of the rmw runs inside it);
  awake     it was entered after the tap: the thread was busy, and the span includes whatever it was doing;
  calls     the system calls that returned on that thread inside [tap, send entry], by name.

Usage: kh2_pong_calls.py <OUT file of rmw_crosshost_rtt.sh> <stem> [<stem> ...]
"""
import collections
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rmw_pcap_split as S  # noqa: E402  (its matching, not a copy)


def main():
    out = Path(sys.argv[1])
    text = out.read_text()
    offsets = {}
    for m in re.finditer(r"clock_offset_ns stem=(\S+) before: client=(-?\d+) server=(-?\d+) "
                         r"after: client=(-?\d+) server=(-?\d+)", text):
        c0, _, c1, _ = (int(x) for x in m.groups()[1:])
        offsets[m.group(1)] = (c0 + c1) // 2
    for stem in sys.argv[2:]:
        coff = offsets.get(stem)
        pc = out.with_suffix(out.suffix + ".pcaps")
        sst = out.with_suffix(out.suffix + ".sysstamp") / f"{stem}_pong"
        if coff is None or not sst.is_dir():
            print(f"== {stem}: no offset or no sysstamp records")
            continue
        ping, pong = pc / f"{stem}_ping.pcap", pc / f"{stem}_pong.pcap"
        sent, cli = S.ping_keys(ping, coff)
        arr = S.first_by_key(pong, sent, cli)
        srv = S.server_ip(ping, sent, cli)
        recs = S.sysstamp_records(sst)
        state = collections.Counter()
        seqs = collections.Counter()
        span = {"asleep": [], "awake": []}
        for k, tap in arr.items():
            kh = k.hex()
            pr = S.first_call(recs, kh, "recv", tap)
            pss = S.first_call(recs, kh, "send", pr[3]) if pr else None
            if pr is None:
                pss = S.first_call(recs, kh, "send", tap)
                pr = S.ring_receive(recs, tap, pss[2], pss[0]) if pss else None
            if not (pr and pss):
                state["unmatched"] += 1
                continue
            st = "asleep" if pr[2] < tap else "awake"
            state[st] += 1
            span[st].append(pr[3] - tap)
            names = [r[1] + ("" if r[4] >= 0 else "(err)") for r in recs if r[0] == pss[0] and tap <= r[3] <= pss[2]]
            seqs[" ".join(names)] += 1
        print(f"== {stem}  (server {srv and '.'.join(str(b) for b in srv)})")
        print(f"   receive call entered before the tap (asleep) / after it (awake): {dict(state)}")
        for st, xs in span.items():
            if xs:
                xs.sort()
                print(f"   tap->recv {st:6s} n={len(xs):4d} p50 {xs[len(xs) // 2] / 1000:6.1f} us")
        for seq, n in seqs.most_common(6):
            print(f"   {n:4d}  calls returned on the reply thread in [tap, send]: {seq}")


if __name__ == "__main__":
    main()
