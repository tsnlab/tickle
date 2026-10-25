"""k5_floor_traj.py - when the least transit (32-request ring, first copy) reaches a value, against the first probe
samples: does a floor converge before the seed?"""
import glob
import os
import sys

RING = 32
for d in sys.argv[1:]:
    for tr in sorted(glob.glob(os.path.join(d, 'runs', '*', 'sub.trace'))):
        ring, got, pend = [], set(), None
        t0, mn, traj, seeds = None, None, [], []
        for line in open(tr, errors='replace'):
            p = line.split()
            if len(p) != 4:
                continue
            k, t, a, b = p[0], int(p[1]), int(p[2]), int(p[3])
            if t0 is None:
                t0 = t
            if k == 'A':
                pend = (t, a, b)
            elif k == 'a' and pend is not None:
                if pend[2] > 0:
                    ring.append((pend[0], pend[1], a + (b >> 8)))
                    if len(ring) > RING:
                        ring.pop(0)
                pend = None
            elif k == 'U' and a not in got:
                got.add(a)
                old = [tn for (tn, lo, hi) in ring if lo <= a <= hi]
                if old:
                    s = (t - min(old)) / 1e6
                    if mn is None or s < mn:
                        mn = s
                        traj.append((round((t - t0) / 1e9, 2), round(s, 1)))
            elif k == 'R' and len(seeds) < 4 and (b >> 8) & 1 and not (b >> 10) & 1:
                seeds.append((round((t - t0) / 1e9, 2), round(a / 1000, 2), mn and round(mn, 1)))
        print(f'{os.path.basename(d)} {os.path.basename(os.path.dirname(tr))}: floor steps {traj[:6]} '
              f'first path probes (t, ms, floor then) {seeds}')
