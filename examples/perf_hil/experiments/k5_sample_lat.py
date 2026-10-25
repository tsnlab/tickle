"""k5_sample_lat.py - each 1 MiB sample's time from its send start (pub.trace C) to its last unit's first arrival
(sub.trace U), per traced rep; prints the slowest samples' index and latency, and the median, to see WHEN in a rep the
tail falls (start-up vs steady state)."""
import glob
import os
import statistics
import sys

for d in sys.argv[1:]:
    for r in sorted(glob.glob(os.path.join(d, 'runs', '*'))):
        pt, st = os.path.join(r, 'pub.trace'), os.path.join(r, 'sub.trace')
        if not (os.path.exists(pt) and os.path.exists(st)):
            continue
        starts = []
        for ln in open(pt, errors='replace'):
            p = ln.split()
            if len(p) == 4 and p[0] == 'C':
                starts.append((int(p[2]), int(p[3]), int(p[1])))
        first = {}
        for ln in open(st, errors='replace'):
            p = ln.split()
            if len(p) == 4 and p[0] == 'U':
                s, n, t = int(p[2]), int(p[3]), int(p[1])
                for u in range(s, s + n):
                    if u not in first:
                        first[u] = t
        lat = []
        for i, (s, n, t0) in enumerate(starts):
            ts = [first.get(u) for u in range(s, s + n)]
            if None in ts:
                continue
            lat.append((max(ts) - t0) / 1e6)
        if not lat:
            continue
        worst = sorted(range(len(lat)), key=lambda i: -lat[i])[:4]
        print(f'{os.path.basename(d)} {os.path.basename(r)}: n {len(lat)} median {statistics.median(lat):.1f} '
              f'worst {[(i, round(lat[i], 1)) for i in worst]} first5 {[round(x, 1) for x in lat[:5]]}')
