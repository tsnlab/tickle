#!/usr/bin/env python3
"""eth_storm_read.py OUT TAG - the reading of eth_storm_census.sh (RESULTS `EWS`), written before the run.

RULES (applied below, in this order):
  REPS: every run dir under OUT/<TAG>_r*_i*/runs/ with a writer RESULT line counts; one without is VOID and listed.
  STORM: the writer's retransmitted >= 100,000 (calm reps on disk resend 2,600-10,000; the storm rep 4,572,542).
  RATE: storms / reps here, with the exact one-sided 95% upper bound (3/N when 0); also pooled with the 1 in 505 on
    disk (main's writer, untraced).
  TRACE CONTROL (round 1, every trace kept): in each round-1 rep whose writer trace did not reach the 2^21-event cap,
    the trace's S count must equal RSTATS retransmitted and its T count RSTATS released_timeout, and a rep that resent
    at all must have K > 0 (Karn samples: some once-resent unit is always passed by the watermark in a calm rep).
    PASS when every such rep agrees; otherwise FAIL and H is not read (the hooks are not on the path).
  H, per STORM rep with a writer trace (the census header's P1-P3):
    t0 = start of the first 100 ms window with >= 200 T events (writer trace);
    P2 K events in [t0, t0 + 2 s] <= 5;
    P3 median over S events in [t0 + 0.5 s, t0 + 2.5 s] of (first reader U/D of that unit after the S - the S's time)
       >= 10 x median timeout of the T events in that window; fewer than 50 matched S -> P3 UNREADABLE (the reader's
       trace capped or missing).
    SUPPORTED when P2 and P3 hold; else NOT SUPPORTED (naming the failed P); UNREADABLE when P3 cannot be read.
  S1 (classified, not part of H): srtt_at_t0 = b of the last K before t0 (us): COLLAPSED < 128 (TVS-LIKE), LOW < the
    1st percentile of every K srtt in the round-1 traces (calm_q01, printed), else CALM; no K before t0: NONE.
  VERDICT: H SUPPORTED if every storm rep read is SUPPORTED; NOT REPRODUCED when there is no storm (the rate bound
    stands for the reading); MIXED otherwise.
  DESCRIPTIVE (not rules): per storm rep, the 2 s before t0 - writer first sends (C), resends (S), timeout releases
    (T), ACKNACKs received (N) and their bits, Y path-failed marks, reader timer ACKNACKs (I) - and the srtt series.
"""
import bisect
import glob
import math
import os
import re
import statistics
import sys

CAP = 1 << 21


def num(text, key):
    m = re.search(r'\b' + key + r'=(\d+)', text)
    return int(m.group(1)) if m else None


def read_trace(path):
    ev = []
    if not os.path.exists(path):
        return None
    with open(path, errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) == 4:
                ev.append((p[0], int(p[1]), int(p[2]), int(p[3])))
    return ev


def upper95(k, n):
    # exact one-sided Clopper-Pearson upper bound
    if n == 0:
        return 1.0
    if k == 0:
        return 1 - 0.05 ** (1 / n)
    lo, hi = k / n, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        cdf = sum(math.comb(n, i) * mid ** i * (1 - mid) ** (n - i) for i in range(k + 1))
        if cdf > 0.05:
            lo = mid
        else:
            hi = mid
    return hi


def analyse_storm(run, out, calm_q01):
    pub = read_trace(run + '/pub.trace')
    sub = read_trace(run + '/sub.trace')
    if not pub:
        out.append('    no writer trace: H not readable')
        return 'UNREADABLE'
    out.append(f'    writer trace {len(pub)} events (cap {"hit" if len(pub) >= CAP else "no"}), '
               f'reader trace {len(sub) if sub else 0}')
    tstart = next((e[1] for e in pub if e[0] == 'C'), pub[0][1])
    ts = [e[1] for e in pub if e[0] == 'T']
    t0 = None
    j = 0
    for i, t in enumerate(ts):
        while ts[j] < t - 100_000_000:
            j += 1
        if i - j + 1 >= 200:
            t0 = ts[j]
            break
    if t0 is None:
        out.append('    no 100 ms window with >= 200 timeout releases: onset not found')
        return 'UNREADABLE'
    rel = lambda t: (t - tstart) / 1e9
    out.append(f'    onset t0 = {rel(t0):.3f} s after the first send')
    ks = [e for e in pub if e[0] == 'K']
    before = [e for e in ks if e[1] < t0]
    s1 = 'NONE'
    if before:
        srtt0 = before[-1][3]
        s1 = 'COLLAPSED' if srtt0 < 128 else 'LOW' if calm_q01 is not None and srtt0 < calm_q01 else 'CALM'
        out.append(f'    S1 srtt at t0 {srtt0} us (calm q01 {calm_q01}; last Karn sample {before[-1][2]} us at '
                   f'{rel(before[-1][1]):.3f} s); least srtt before t0 {min(e[3] for e in before)} us; K before t0 '
                   f'{len(before)} -> {s1}')
        out.append(f'      last 20 K before t0 (sample/srtt us): {[(e[2], e[3]) for e in before[-20:]]}')
    else:
        out.append('    S1 no Karn sample before t0 -> NONE')
    k2 = sum(1 for e in ks if t0 <= e[1] < t0 + 2_000_000_000)
    p2 = k2 <= 5
    out.append(f'    P2 Karn samples in [t0, t0+2s] {k2} (after t0 in all {sum(1 for e in ks if e[1] >= t0)}) -> '
               f'{"PASS" if p2 else "FAIL"}')
    w0, w1 = t0 + 500_000_000, t0 + 2_500_000_000
    tw = [e for e in pub if e[0] == 'T' and w0 <= e[1] < w1]
    tmo = statistics.median([e[3] & 0xFFFFFF for e in tw]) if tw else None
    multi = sum(1 for e in tw if (e[3] >> 24) >= 2)
    p3 = None
    if sub and tw:
        arr = {}
        for e in sub:
            if e[0] in ('U', 'D'):
                cnt = e[3] if e[0] == 'U' else 1
                for q in range(e[2], e[2] + max(cnt, 1)):
                    arr.setdefault(q, []).append(e[1])
        owd = []
        for e in pub:
            if e[0] == 'S' and w0 <= e[1] < w1:
                a = arr.get(e[2])
                if a:
                    k = bisect.bisect_right(a, e[1])
                    if k < len(a):
                        owd.append((a[k] - e[1]) / 1000)
        if len(owd) >= 50:
            m = statistics.median(owd)
            p3 = m >= 10 * tmo
            q = sorted(owd)
            out.append(f'    P3 repairs resent in [t0+0.5, t0+2.5 s]: {len(owd)} matched, one-way median {m:.0f} us '
                       f'(p10 {q[len(q) // 10]:.0f}, p90 {q[9 * len(q) // 10]:.0f}) against median timeout {tmo:.0f} '
                       f'us ({len(tw)} T, {multi} of them a unit resent >= 2 times) -> {"PASS" if p3 else "FAIL"}')
        else:
            out.append(f'    P3 only {len(owd)} matched repairs (reader trace capped or missing) -> UNREADABLE '
                       f'(median timeout {tmo} us, {len(tw)} T, {multi} resent >= 2)')
    else:
        out.append(f'    P3 no reader trace or no T in the window -> UNREADABLE')
    # descriptive: the 2 s before t0, in 250 ms bins
    out.append('    before t0 (250 ms bins, t rel. first send): C S T N bits Y | reader I U')
    for b in range(-8, 2):
        lo, hi = t0 + b * 250_000_000, t0 + (b + 1) * 250_000_000
        c = {k: 0 for k in 'CSTNY'}
        bits = 0
        for e in pub:
            if lo <= e[1] < hi and e[0] in c:
                c[e[0]] += 1
                if e[0] == 'N':
                    bits += e[3]
        ri = sum(1 for e in sub or [] if e[0] == 'I' and lo <= e[1] < hi)
        ru = sum(e[3] for e in sub or [] if e[0] == 'U' and lo <= e[1] < hi)
        out.append(f'      {rel(lo):7.2f} C {c["C"]:3} S {c["S"]:6} T {c["T"]:6} N {c["N"]:4} bits {bits:7} '
                   f'Y {c["Y"]} | I {ri:4} U {ru:6}')
    ys = [rel(e[1]) for e in pub if e[0] == 'Y']
    out.append(f'    Y marks at {["%.2f" % y for y in ys]} s; srtt series (every 20th K): '
               f'{[e[3] for e in ks[::20]][:40]}')
    if p3 is None:
        return 'UNREADABLE S1=' + s1
    if p2 and p3:
        return 'SUPPORTED S1=' + s1
    return 'S1=' + s1 + ' NOT SUPPORTED (' + ','.join(n for n, v in (('P2', p2), ('P3', p3)) if not v) + ')'


def main():
    base, tag = sys.argv[1], sys.argv[2]
    out = []
    reps, void, storms, ctl = [], [], [], []
    for d in sorted(glob.glob(f'{base}/{tag}_r*_i*/')):
        rnd = int(re.search(r'_r(\d+)_i', d).group(1))
        for run in sorted(glob.glob(d + 'runs/*')):
            try:
                pl = open(run + '/ping.log', errors='replace').read()
            except OSError:
                void.append(run)
                continue
            if not re.search(r'^RESULT: role=pub', pl, re.M):
                void.append(run)
                continue
            retx = num(re.search(r'^RESULT: role=pub.*$', pl, re.M).group(0), 'retransmitted')
            rt = num(pl, 'released_timeout')
            rack = num(pl, 'rack')
            reps.append((run, retx))
            if retx >= 100_000:
                storms.append(run)
            if rnd == 1:
                ctl.append((run, retx, rt, rack))
    n = len(reps)
    out.append(f'REPS {n} (void {len(void)}){"; void: " + " ".join(void[:10]) if void else ""}')
    calm = sorted(r for _, r in reps if r < 100_000)
    if calm:
        out.append(f'  calm resends median {statistics.median(calm):.0f} (range {calm[0]}-{calm[-1]}); untraced main on '
                   'disk: median ~4,300-5,900 by tree')
    out.append(f'STORM {len(storms)} of {n}: ' + ' '.join(os.path.relpath(s, base) for s in storms))
    k = len(storms)
    out.append(f'RATE here {k}/{n} = {k / n if n else 0:.4%}, 95% upper {upper95(k, n):.4%}; pooled with disk '
               f'{k + 1}/{n + 505} = {(k + 1) / (n + 505):.4%}, 95% upper {upper95(k + 1, n + 505):.4%}')
    # trace control
    bad, read = [], 0
    for run, retx, rt, rack in ctl:
        ev = read_trace(run + '/pub.trace')
        if ev is None:
            bad.append(f'{os.path.relpath(run, base)} no trace')
            continue
        if len(ev) >= CAP:
            continue
        read += 1
        s = sum(1 for e in ev if e[0] == 'S')
        t = sum(1 for e in ev if e[0] == 'T')
        kk = sum(1 for e in ev if e[0] == 'K')
        if s != retx or t != rt or (retx and kk == 0):
            bad.append(f'{os.path.relpath(run, base)} S {s}/{retx} T {t}/{rt} K {kk}')
    tc = 'PASS' if read > 0 and not bad else 'FAIL'
    calm_srtt = []
    for run, retx, rt, rack in ctl:
        if retx < 100_000:
            calm_srtt += [e[3] for e in read_trace(run + '/pub.trace') or [] if e[0] == 'K']
    calm_srtt.sort()
    calm_q01 = calm_srtt[len(calm_srtt) // 100] if calm_srtt else None
    if calm_srtt:
        out.append(f'  calm srtt (round-1 K, {len(calm_srtt)}): min {calm_srtt[0]} q01 {calm_q01} median '
                   f'{calm_srtt[len(calm_srtt) // 2]} us')
    out.append(f'TRACE CONTROL {tc}: {read} round-1 reps read, {len(bad)} disagree ' + '; '.join(bad[:10]))
    verdicts = []
    for s in storms:
        out.append(f'  storm {os.path.relpath(s, base)}:')
        pl = open(s + '/ping.log', errors='replace').read()
        for key in ('retransmitted', 'released_timeout', 'null_evicted', 'published', 'acknack_received'):
            out.append(f'    {key} {num(pl, key)}')
        v = analyse_storm(s, out, calm_q01) if tc == 'PASS' else 'NOT READ (trace control)'
        out.append(f'    -> {v}')
        verdicts.append(v)
    if not storms:
        out.append(f'H not read: no storm in {n} reps')
        out.append(f'VERDICT NOT REPRODUCED in {n} reps (p < {upper95(0, n):.4%} at 95% on this tree)')
    elif all(v.startswith('SUPPORTED') for v in verdicts):
        out.append(f'VERDICT H SUPPORTED in {len(verdicts)} of {len(verdicts)} storm reps')
    else:
        out.append('VERDICT MIXED: ' + ', '.join(verdicts))
    print('\n'.join(out))


if __name__ == '__main__':
    main()
