#!/usr/bin/env python3
"""C12 ping side: which thread receives each echo, by which call, and tap -> receive return per path.

Per echo (ping host's tap time R, first copy from the server): the receive is the first recv* record carrying the key
that returned at or after R (epoll+recvfrom path; its thread and the epoll_pwait2 before it on that thread), else the
first successful io_uring_enter returning at or after R (ring path). Also per run: threads and their call counts,
ring owner changes (tid of consecutive io_uring_enter records), and per echo cycle the waits the ring owner made
between two consecutive executor epoll_pwait2 calls (what resets wr_streak).

Usage: ping_path.py <OUT .txt of kh2_rig_ab> <stem> [<stem> ...]   (rmw_tickle@libc_* or vendor stems)
"""
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import rmw_pcap_split as S  # noqa: E402


def q(xs, f):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * f))]


def line(label, xs):
    if not xs:
        return f"   {label:30s} n=   0"
    return f"   {label:30s} n={len(xs):4d} p25 {q(xs, .25):6.2f} p50 {q(xs, .5):6.2f} p75 {q(xs, .75):6.2f} us"


out = Path(sys.argv[1])
text = out.read_text()
offs = {}
for m in re.finditer(r"clock_offset_ns stem=(\S+) before: client=(-?\d+) server=(-?\d+) "
                     r"after: client=(-?\d+) server=(-?\d+)", text):
    c0, _, c1, _ = (int(x) for x in m.groups()[1:])
    offs[m.group(1)] = (c0 + c1) // 2
for stem in sys.argv[2:]:
    pc = out.with_suffix(out.suffix + ".pcaps")
    sst = out.with_suffix(out.suffix + ".sysstamp") / f"{stem}_ping"
    if stem not in offs or not sst.is_dir():
        print(f"== {stem}: NO OFFSET OR NO SYSSTAMP DIR")
        continue
    sent, cli = S.ping_keys(pc / f"{stem}_ping.pcap", offs[stem])
    srv = S.server_ip(pc / f"{stem}_ping.pcap", sent, cli)
    back = S.first_by_key(pc / f"{stem}_ping.pcap", sent, srv)
    recs = S.sysstamp_records(sst)
    per = Counter((r[0], r[1]) for r in recs)
    paths = {"recvfrom": [], "ring": []}
    split = {"tap->epoll return": [], "epoll return->recvfrom return": []}
    recv_tid = Counter()
    waited = Counter()
    for k, tap in back.items():
        r = S.first_call(recs, k.hex(), "recv", tap)
        if r:
            if r[2] >= tap and not S.wake_before(recs, r, tap):
                waited["awake (recv entered after tap, no wait)"] += 1
                continue
            w = S.wake_before(recs, r, tap)
            waited[f"{w[1] if w else 'none'} -> {r[1]}"] += 1
            if w:
                split["tap->epoll return"].append((w[3] - tap) / 1000)
                split["epoll return->recvfrom return"].append((r[3] - w[3]) / 1000)
            recv_tid[r[0]] += 1
            paths["recvfrom"].append((r[3] - tap) / 1000)
            continue
        r = S.ring_next(recs, tap)
        if r:
            recv_tid[r[0]] += 1
            waited["io_uring_enter"] += 1
            paths["ring"].append((r[3] - tap) / 1000)
    ring = [r for r in recs if r[1] == S.RING]
    changes = sum(1 for a, b in zip(ring, ring[1:]) if a[0] != b[0])
    owners = Counter(r[0] for r in ring)
    # owner waits between consecutive executor waits (executor = the thread that received most echoes)
    ex = recv_tid.most_common(1)[0][0] if recv_tid else None
    exw = [r for r in recs if r[0] == ex and r[1] == "epoll_pwait2"]
    between = Counter()
    for a, b in zip(exw, exw[1:]):
        between[sum(1 for r in ring if r[0] != ex and a[3] < r[2] < b[2])] += 1
    print(f"== {stem}  (echoes at ping tap {len(back)}, sysstamp records {len(recs)})")
    print(f"   threads: " + "; ".join(f"{t}:" + ",".join(f"{c}={n}" for (tt, c), n in sorted(per.items()) if tt == t)
                                     for t in sorted({t for t, _ in per})))
    print(f"   echo received on thread {dict(recv_tid)}; by {dict(waited)}")
    print(f"   io_uring_enter by thread {dict(owners)}; owner changes {changes}")
    if ex:
        print(f"   ring-owner io_uring_enter calls between two executor epoll_pwait2 entries: {dict(sorted(between.items()))}")
    paths["all"] = paths["recvfrom"] + paths["ring"]
    for p, xs in list(paths.items()) + list(split.items()):
        print(line(f"tap->return via {p}", xs))
