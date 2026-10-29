#!/usr/bin/env python3
"""C12 diagnostic: pong tap -> receive return, per rep.

TickLE (ring arms): grouped by when the reply thread last sent before entering the wait that received the ping.
Vendors (cyclonedds/fastrtps): the recv* carrying the ping's key, for comparison from the same records.

Only stems whose pong sysstamp records the hot calls can be read: rmw_tickle@libc (TT_HAL_LIBC_CALLS). The
rmw_tickle@default build makes raw system calls that the sysstamp shim does not see (7 records a run), so its
stems yield nothing; the script says so instead of printing nothing.

Usage: prev_call.py <OUT .txt of kh2_rig_ab> <stem> [<stem> ...]
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import rmw_pcap_split as S  # noqa: E402


def p(xs, q):
    return xs[min(len(xs) - 1, int(len(xs) * q))]


def line(label, xs):
    if not xs:
        return f"   {label:34s} n=   0"
    xs = sorted(xs)
    return f"   {label:34s} n={len(xs):4d} p25 {p(xs, .25):6.2f} p50 {p(xs, .5):6.2f} p75 {p(xs, .75):6.2f} us"


out = Path(sys.argv[1])
text = out.read_text()
offs = {}
for m in re.finditer(r"clock_offset_ns stem=(\S+) before: client=(-?\d+) server=(-?\d+) "
                     r"after: client=(-?\d+) server=(-?\d+)", text):
    c0, _, c1, _ = (int(x) for x in m.groups()[1:])
    offs[m.group(1)] = (c0 + c1) // 2
for stem in sys.argv[2:]:
    pc = out.with_suffix(out.suffix + ".pcaps")
    sst = out.with_suffix(out.suffix + ".sysstamp") / f"{stem}_pong"
    if stem not in offs or not sst.is_dir():
        print(f"== {stem}: NO OFFSET OR NO SYSSTAMP DIR")
        continue
    sent, cli = S.ping_keys(pc / f"{stem}_ping.pcap", offs[stem])
    arr = S.first_by_key(pc / f"{stem}_pong.pcap", sent, cli)
    recs = S.sysstamp_records(sst)
    vendor = not stem.startswith("rmw_tickle")
    groups = {"all": [], "send<1ms": [], "send 1-20ms": [], "send>=20ms": []}
    skipped = 0
    for k, tap in arr.items():
        if vendor:
            pr = S.first_call(recs, k.hex(), "recv", tap)
        else:
            pss = S.first_call(recs, k.hex(), "send", tap)
            pr = S.ring_receive(recs, tap, pss[2], pss[0]) if pss else None
        if not pr or pr[2] >= tap:  # unmatched, or entered after the tap (awake)
            skipped += 1
            continue
        v = (pr[3] - tap) / 1000
        groups["all"].append(v)
        if vendor:
            continue
        prior = [r for r in recs if r[0] == pr[0] and r[3] <= pr[2]]
        lastsend = next((r for r in reversed(prior) if r[1].startswith("send")), None)
        gap_ms = (pr[2] - lastsend[3]) / 1e6 if lastsend else 1e9
        groups["send<1ms" if gap_ms < 1 else "send 1-20ms" if gap_ms < 20 else "send>=20ms"].append(v)
    print(f"== {stem}  (pings at pong tap {len(arr)}, sysstamp records {len(recs)}, skipped unmatched/awake {skipped})")
    if not groups["all"]:
        print("   NO SAMPLE MATCHED - this stem's sysstamp holds no hot calls (an @default build?)")
        continue
    print(line("tap->recv all (asleep)", groups["all"]))
    if not vendor:
        for g in ("send<1ms", "send 1-20ms", "send>=20ms"):
            print(line(f"last send before wait {g}", groups[g]))
