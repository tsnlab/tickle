"""k5_ambig.py - was each timed recovery's unit asked for before its probe began? (sub.trace, r4_trace + add_r_event)

For each R (a probe ended: took_us, flags) the unit is the next U/X event's seq. Every A/a pair is one ACKNACK naming
[A.a, a.a + (a.b >> 8)] with A.b bits (0 bits = names nothing). The probe began at R.t - took. AMBIGUOUS: an ACKNACK
naming the unit went before the probe began (its repair may answer that one). Prints, per rep, the timed samples
(path, not busy) split into unambiguous / ambiguous: count, min, median; and how many are under 3 ms (below the
path's ~4 ms floor: netem delay 2 ms each way).
"""
import bisect
import glob
import os
import statistics
import sys


def analyse(path):
    ev = []
    with open(path, errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) == 4 and p[0] in 'RAaUX':
                ev.append((p[0], int(p[1]), int(p[2]), int(p[3])))
    names = []  # (t, lo, hi) of every ACKNACK naming something
    pend = None
    out = []
    for i, (k, t, a, b) in enumerate(ev):
        if k == 'A':
            pend = (t, a, b)
        elif k == 'a' and pend is not None:
            if pend[2] > 0:
                names.append((pend[0], pend[1], a + (b >> 8)))
            pend = None
        elif k == 'R':
            seq = None
            for k2, t2, a2, b2 in ev[i + 1:i + 6]:
                if k2 in 'UX':
                    seq = a2
                    break
            retry, path_, busy = b & 0xff, (b >> 8) & 1, (b >> 10) & 1
            start = t - a * 1000
            first = None
            for (tn, lo, hi) in names:
                if lo <= seq <= hi:
                    first = tn
                    break
            amb = first is not None and first < start - 1000
            from_first = (t - first) / 1e6 if first is not None else None
            out.append(dict(t=t, took=a / 1000, retry=retry, path=path_, busy=busy, amb=amb, seq=seq,
                            from_first=from_first))
    return out


def summ(v):
    return f'n {len(v)} min {min(v):.2f} med {statistics.median(v):.2f} under3ms {sum(1 for x in v if x < 3)}' if v else 'n 0'


for d in sys.argv[1:]:
    for tr in sorted(glob.glob(os.path.join(d, 'runs', '*', 'sub.trace'))):
        s = analyse(tr)
        name = os.path.basename(os.path.dirname(tr))
        timed = [x for x in s if x['path'] and not x['busy']]
        un = [x['took'] for x in timed if not x['amb']]
        am = [x['took'] for x in timed if x['amb']]
        allun = [x['took'] for x in s if not x['amb']]
        allam = [x['took'] for x in s if x['amb']]
        ff = [x['from_first'] for x in timed if x['amb'] and x['from_first'] is not None]
        print(f'{os.path.basename(d)} {name}: probes {len(s)} timed {len(timed)}')
        print(f'   timed unambiguous ms: {summ(un)}')
        print(f'   timed ambiguous ms:   {summ(am)}; same timed from the first request: {summ(ff)}')
        print(f'   all probes unambiguous: {summ(allun)} | ambiguous: {summ(allam)}')
