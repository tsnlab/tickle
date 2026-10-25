"""k5_transit_min.py - the least repair transit a reader would see (note_repair_arrival(): from the OLDEST of its last
32 requests naming the unit, first copy only), against the least from the first-ever request naming it. sub.trace.
A floor for the retry estimate is sound only if the ring's least stays near the path's round trip (>= ~4 ms here)."""
import glob
import os
import sys

RING = 32
for d in sys.argv[1:]:
    for tr in sorted(glob.glob(os.path.join(d, 'runs', '*', 'sub.trace'))):
        ring = []  # (t, lo, hi), at most RING
        first_named = {}
        got = set()
        mins = []
        mins_first = []
        pend = None
        with open(tr, errors='replace') as f:
            for line in f:
                p = line.split()
                if len(p) != 4:
                    continue
                k, t, a, b = p[0], int(p[1]), int(p[2]), int(p[3])
                if k == 'A':
                    pend = (t, a, b)
                elif k == 'a' and pend is not None:
                    if pend[2] > 0:
                        lo, hi = pend[1], a + (b >> 8)
                        ring.append((pend[0], lo, hi))
                        if len(ring) > RING:
                            ring.pop(0)
                    pend = None
                elif k == 'U':
                    if a in got:
                        continue
                    got.add(a)
                    old = [tn for (tn, lo, hi) in ring if lo <= a <= hi]
                    if old:
                        mins.append((t - min(old)) / 1e6)
        mins.sort()
        name = os.path.basename(os.path.dirname(tr))
        print(f'{os.path.basename(d)} {name}: transit samples {len(mins)} least5 ms {[round(x, 2) for x in mins[:5]]} '
              f'p1 {mins[len(mins) // 100]:.2f} median {mins[len(mins) // 2]:.2f}' if mins else f'{name}: none')
