#!/usr/bin/env python3
"""RESULTS same-host S11 (publisher CPU, BEST_EFFORT P4, max rate) on goodput: the publisher's CPU per DELIVERED sample.

Usage: s11_publisher_cpu.py <OUTB> [<CONTROL_OUTB>]

<OUTB> is a fair_samehost_remeasure.sh run with CELLS=best_effort_throughput:p4; the files read are
<OUTB>.best_effort_throughput_p4_{free,pinned}.txt. <CONTROL_OUTB> is an earlier run of the same cell (the e521bbba
campaign, ~/rig_results_safe/fair_samehost_e521bbba_20261009-033254) whose vendor arms are the control.

Why it exists: S11 was published (e17b4e6f, p4_shm_headtohead_e17b4e6f.txt) as the publisher's cpu_s_per_Msample - CPU
over samples SENT - and that file has no FastDDS receive count, so it could not be put on TESTING.md section 4's
denominator (CPU per delivered sample) when goodput replaced the send rate on 2026-10-17.

HOW TO READ IT, fixed before the run (2026-10-17), and enforced below rather than only stated here:
  - Only each framework's ON arm (its own shared-memory transport). A rep counts when it has the client's RESULT line
    with utime_s and stime_s, and the server's RESULT line with recv (s6_reps.py pairs them). A rep with no server
    line is not a zero: it does not count. Fewer than 2 counted reps for a framework -> that framework reads n/a.
  - Per rep: publisher_us = (client utime_s + stime_s) / server recv * 1e6. Both are whole-run figures (the client's
    whole-process CPU, the server's whole-run count of samples its application took complete), so numerator and
    denominator cover the same lifetime. A rep with recv = 0 reads infinity (CPU spent, nothing delivered) and is
    counted, not dropped: dropping it would score a framework only on the reps it happened to deliver in.
  - CycloneDDS's shared memory is the daemon iox-roudi plus the two processes, so its scored S11 figure is
    (client + iox-roudi CPU) / recv; its publisher-only figure is printed beside it as a diagnostic. A CycloneDDS rep
    with no ROUDI line does not count. (The old row compared against FastDDS only because the daemon was not counted.)
  - BEST_EFFORT is scored on goodput alone (TESTING.md section 4): no DELIVERY FAILED for anyone; a sample not
    delivered is a sample not in the denominator.
  - The figure is the median over counted reps of the free (unpinned) arm. The pinned arm is printed as a note.
  - Win rule: TickLE wins when its WORST counted rep is below every scored vendor's BEST counted rep (outside the
    spread); TickLE loses when its best rep is above some vendor's worst; otherwise DRAW.
  - VOID (the row is not published) when: a framework's s6 witness says the ON arm was not carried by shared memory
    (TickLE and CycloneDDS: ratio > 0.25; FastDDS's kernel-path arm reads 0 packets on lo, so its witness is VOID by
    construction, as in every published same-host row, and is not a cause here); a counted rep has client CPU 0 (an
    instrument read zero); the BEST_EFFORT history echo is not history_arg=1 on every framework.
  - Control, the arm this re-measure cannot touch: each VENDOR's publisher CPU per SENT sample (client_us, free arm)
    against the same figure in <CONTROL_OUTB>. Outside 0.80x..1.25x -> CONTROL DRIFT and the row is VOID: the rig is not
    in the state the published rows were measured in. TickLE is not a control (its code moved since e521bbba).
    Without <CONTROL_OUTB> the run cannot be published and says so.
Exit status: 0 publishable, 3 VOID, 2 usage.
"""
import math
import os
import re
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s6_reps  # noqa: E402 - beside this file, found through the path set just above

FRAMEWORKS = ('tickle', 'fastdds', 'cyclonedds')
CELL = 'best_effort_throughput_p4'
CONTROL_BAND = (0.80, 1.25)
WITNESS_MAX = 0.25


def cell_path(outb, arm):
    return f'{outb}.{CELL}_{arm}.txt'


def cpu(f):
    u, s = s6_reps.num(f, 'utime_s'), s6_reps.num(f, 'stime_s')
    return None if u is None or s is None else u + s


def rep_figures(rep):
    """(sent, recv, client_cpu_s, roudi_cpu_s, publisher_us, scored_us) or None when the rep does not count."""
    c, s, r = rep['client'], rep['server'], rep['roudi']
    c_cpu, recv, sent = cpu(c), s6_reps.num(s, 'recv'), s6_reps.num(c, 'sent')
    if c_cpu is None or recv is None or sent is None:
        return None
    r_cpu = None
    if rep['fw'] == 'cyclonedds':
        if r is None or r.get('state') != 'ok':
            return None
        r_cpu = s6_reps.num(r, 'sched_cpu_s')
        if r_cpu is None:
            r_cpu = s6_reps.num(r, 'cpu_s')
        if r_cpu is None:
            return None
    pub = c_cpu / recv * 1e6 if recv > 0 else math.inf
    scored = (c_cpu + (r_cpu or 0.0)) / recv * 1e6 if recv > 0 else math.inf
    return sent, recv, c_cpu, r_cpu, pub, scored


def counted(reps, fw):
    out = []
    for rep in reps:
        if rep['fw'] == fw and rep['arm'] == 'ON':
            fig = rep_figures(rep)
            if fig is not None:
                out.append((rep, fig))
    return out


def witness_ratios(path):
    """{fw: ratio or 'VOID'} from s6_transport_cells.sh's own verdict block."""
    out, fw = {}, None
    for line in open(path, errors='replace'):
        m = re.match(r'^  (\w+): ON n=', line)
        if m:
            fw = m.group(1)
            continue
        m = re.match(r'^    ratio ([0-9.]+)\s', line)
        if m and fw:
            out[fw] = float(m.group(1))
        elif fw and line.startswith(f'    VOID {fw}'):
            out[fw] = 'VOID'
    return out


def fmt(x, digits=3):
    return 'inf' if x == math.inf else f'{x:.{digits}f}'


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__.splitlines()[2], file=sys.stderr)
        return 2
    outb = sys.argv[1]
    control = sys.argv[2] if len(sys.argv) == 3 else None
    voids = []
    results = {}
    for arm in ('free', 'pinned'):
        path = cell_path(outb, arm)
        if not os.path.exists(path):
            if arm == 'free':
                print(f'VOID: no {path}')
                return 3
            continue
        reps, orphans = s6_reps.read_reps(path)
        print(f'\n{arm} arm: {path} (orphan lines {orphans})')
        wit = witness_ratios(path)
        for fw in FRAMEWORKS:
            rs = counted(reps, fw)
            for rep, (sent, recv, c_cpu, r_cpu, pub, scored) in rs:
                hist = rep['client'].get('history_arg')
                extra = f' roudi_cpu_s={r_cpu:.4f}' if r_cpu is not None else ''
                print(f'  {fw}: sent={sent:.0f} recv={recv:.0f} ({recv / sent:.6f}) client_cpu_s={c_cpu:.3f}{extra} '
                      f'publisher_us_per_delivered={fmt(pub)} scored_us={fmt(scored)} '
                      f'client_us_per_sent={s6_reps.fmt(s6_reps.num(rep["client"], "cpu_s_per_Msample"))} '
                      f'history_arg={hist}')
                if arm == 'free' and c_cpu <= 0:
                    voids.append(f'{fw}: a rep read client CPU 0')
                if arm == 'free' and hist != '1':
                    voids.append(f'{fw}: history_arg={hist}, not 1')
            if len(rs) < 2:
                print(f'  {fw}: n/a ({len(rs)} counted reps)')
                results.setdefault(arm, {})[fw] = None
                continue
            scored = [f[5] for _, f in rs]
            pub = [f[4] for _, f in rs]
            per_sent = [s6_reps.num(r['client'], 'cpu_s_per_Msample') for r, _ in rs]
            results.setdefault(arm, {})[fw] = {
                'median': st.median(scored), 'lo': min(scored), 'hi': max(scored), 'n': len(rs),
                'pub_median': st.median(pub), 'per_sent': st.median([x for x in per_sent if x is not None]),
                'sent': [f[0] for _, f in rs], 'recv': [f[1] for _, f in rs]}
            w = wit.get(fw)
            print(f'  {fw} n={len(rs)}: S11 scored median {fmt(st.median(scored))} us/delivered '
                  f'(range {fmt(min(scored))}..{fmt(max(scored))}); publisher-only {fmt(st.median(pub))}; '
                  f'per sent {results[arm][fw]["per_sent"]:.3f}; witness {w}')
            if arm == 'free' and fw != 'fastdds' and (w is None or w == 'VOID' or w > WITNESS_MAX):
                voids.append(f'{fw}: witness {w} - the ON arm is not shown to be shared memory')
    free = results.get('free', {})
    if control is None:
        voids.append('no CONTROL_OUTB given: the vendor control was not checked')
    else:
        creps, _ = s6_reps.read_reps(cell_path(control, 'free'))
        for fw in ('fastdds', 'cyclonedds'):
            base = [s6_reps.num(r['client'], 'cpu_s_per_Msample') for r, _ in counted(creps, fw)]
            base = [x for x in base if x is not None]
            now = free.get(fw)
            if len(base) < 2 or now is None:
                voids.append(f'control {fw}: cannot be formed (control n={len(base)}, now {now is not None})')
                continue
            ratio = now['per_sent'] / st.median(base)
            ok = CONTROL_BAND[0] <= ratio <= CONTROL_BAND[1]
            print(f'control {fw}: publisher us per SENT sample {now["per_sent"]:.3f} now vs {st.median(base):.3f} '
                  f'in the control run = {ratio:.3f}x {"ok" if ok else "CONTROL DRIFT"}')
            if not ok:
                voids.append(f'control {fw}: {ratio:.3f}x outside {CONTROL_BAND}')
    t = free.get('tickle')
    vendors = {fw: v for fw, v in free.items() if fw != 'tickle' and v is not None}
    if t is None or not vendors:
        verdict = 'n/a'
    elif all(t['hi'] < v['lo'] for v in vendors.values()):
        verdict = 'WIN'
    elif any(t['lo'] > v['hi'] for v in vendors.values()):
        verdict = 'LOSE'
    else:
        verdict = 'DRAW'
    print(f'\nS11 verdict (free arm): TickLE {verdict}')
    for v in voids:
        print(f'VOID: {v}')
    print('S11 PUBLISHABLE' if not voids else 'S11 NOT PUBLISHABLE')
    return 0 if not voids else 3


if __name__ == '__main__':
    sys.exit(main())
