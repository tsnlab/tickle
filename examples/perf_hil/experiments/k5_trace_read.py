"""k5_trace_read.py - the reader's retry estimate in a traced rep (sub.trace from r4_trace.patch + add_r_event.py).

Events used: R took_us flags (retry | path<<8 | plain<<9 | busy<<10 | srtt_before_us<<11), r srtt_us rttvar_us,
I interval_us retry (each timer ACKNACK). Prints per rep: timed samples (path and not busy), the seed, the srtt after
each of the first timed samples, srtt / interval at the end, the interval's median per 5 s, timer ACKNACKs.
"""
import glob
import os
import statistics
import sys


def read(path):
    ev = []
    with open(path, errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) == 4 and p[0] in 'RrIA':
                ev.append((p[0], int(p[1]), int(p[2]), int(p[3])))
    return ev


def one(path):
    ev = read(path)
    if not ev:
        return None
    t0 = ev[0][1]
    timed, allr = [], []
    last_r = None
    for k, t, a, b in ev:
        if k == 'R':
            retry, path_, plain, busy, before = b & 0xff, (b >> 8) & 1, (b >> 9) & 1, (b >> 10) & 1, b >> 11
            allr.append((t, a, retry, path_, plain, busy, before))
            last_r = (path_, busy)
        elif k == 'r' and last_r is not None:
            if last_r[0] and not last_r[1]:
                timed.append(((t - t0) / 1e9, allr[-1][1], allr[-1][2], a, b))
            last_r = None
    intervals = [((t - t0) / 1e9, a) for k, t, a, b in ev if k == 'I']
    return timed, allr, intervals


def main():
    for d in sys.argv[1:]:
        for tr in sorted(glob.glob(os.path.join(d, 'runs', '*', 'sub.trace'))):
            r = one(tr)
            name = os.path.basename(os.path.dirname(tr))
            if r is None:
                print(name, 'empty trace')
                continue
            timed, allr, iv = r
            n_retry0 = sum(1 for x in allr if x[2] == 0)
            busy = sum(1 for x in allr if x[5])
            first = ' '.join(f'{t:.1f}s:{took}us(r{rt})->{s}/{v}' for t, took, rt, s, v in timed[:6])
            end = f'{timed[-1][3]}/{timed[-1][4]}' if timed else '-'
            bins = {}
            for t, a in iv:
                bins.setdefault(int(t // 5), []).append(a)
            ivs = ' '.join(f'{int(statistics.median(v))}' for k, v in sorted(bins.items()))
            print(f'{name}: probes_ended {len(allr)} (retry0 {n_retry0}, busy {busy}) timed {len(timed)} '
                  f'srtt/rttvar_end_us {end} timerACKs {len(iv)}')
            print(f'   first timed: {first}')
            print(f'   timer interval us, median per 5 s: {ivs}')


main()
