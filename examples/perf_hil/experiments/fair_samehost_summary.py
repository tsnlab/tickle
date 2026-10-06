#!/usr/bin/env python3
"""Reads fair_samehost_remeasure.sh's output and prints the free (unpinned) headline beside the pinned note, then what
each framework's processes cost and what they delivered.

Usage: fair_samehost_summary.py <OUTB>

Rules, as in the published table and fixed before the run:
  - only each framework's ON arm (its own shared-memory transport) is read, and only reps s6_transport_cells.sh
    printed a RESULT for;
  - throughput is the client's win_send_mbps (send_mbps on a line from before the measured window), latency
    rtt_avg_ms, medians of the reps; a rep whose window= is not ok has no rate;
  - a TickLE BEST_EFFORT rep that dropped at a full ring (shm_full_dropped > 0) is excluded, as for S1-S3;
  - a row's leader is decided on the free arm alone; a cell with fewer than 2 usable reps for a framework reads n/a.

Cost and delivery (2026-10-06, ROADMAP "Fill COMPARISON 1a's empty cells"): for the free arm, per framework, the
medians s6_reps.py defines - client, server and (CycloneDDS) iox-roudi CPU per sample, their sum per DELIVERED sample,
peak RSS of each and their sum, and on the throughput cells the server's delivered rate (win_recv_mbps) beside the
client's send rate. Over the same reps as the headline. The headline itself is unchanged: S1-S6 are still send rates,
and the delivered column is printed beside them, not instead of them, until the user decides how the rows read.

Every line is read from <OUTB>.<scenario>_<size>_<arm>.txt, where s6_transport_cells.sh copies the TickLE cell's
lines (indented) beside the vendors'; the .txt.tickle files hold the same TickLE lines and are not read again.
"""
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s6_reps  # noqa: E402 - beside this file, found through the path set just above

outb = sys.argv[1]
SCENS = ('best_effort_throughput', 'reliable_throughput', 'reliable_latency')
FRAMEWORKS = ('tickle', 'fastdds', 'cyclonedds')
reps = {}  # (scen, size, arm) -> [rep]
for scen in SCENS:
    for size in ('p2', 'p3', 'p4'):
        for arm in ('free', 'pinned'):
            path = f'{outb}.{scen}_{size}_{arm}.txt'
            if os.path.exists(path):
                reps[(scen, size, arm)] = s6_reps.read_reps(path)[0]


def headline(rep, scen):
    """The published figure of one rep, or None if the rep does not count."""
    c = rep['client']
    if scen == 'reliable_latency':
        if (s6_reps.num(c, 'recv') or 0) == 0:
            return None
        return s6_reps.num(c, 'rtt_avg_ms')
    if not s6_reps.window_ok(c):
        return None
    if rep['fw'] == 'tickle' and scen == 'best_effort_throughput' and (s6_reps.num(c, 'shm_full_dropped') or 0) > 0:
        return None
    return s6_reps.num(c, 'win_send_mbps') if 'win_send_mbps' in c else s6_reps.num(c, 'send_mbps')


def counted(scen, size, arm, fw):
    """The ON-arm reps of one framework that the headline counts."""
    return [r for r in reps.get((scen, size, arm), [])
            if r['fw'] == fw and r['arm'] == 'ON' and headline(r, scen) is not None]


def med(scen, size, arm, fw):
    v = [headline(r, scen) for r in counted(scen, size, arm, fw)]
    return st.median(v) if len(v) >= 2 else None


def leader(values, scen):
    have = {fw: v for fw, v in values.items() if v is not None}
    if not have:
        return 'n/a'
    return min(have, key=have.get) if scen == 'reliable_latency' else max(have, key=have.get)


for scen in SCENS:
    unit = 'ms RTT, lower is better' if scen == 'reliable_latency' else 'send Mbps, higher is better'
    print(f'\n{scen} ({unit})')
    for size in ('p2', 'p3', 'p4'):
        free = {fw: med(scen, size, 'free', fw) for fw in FRAMEWORKS}
        pinned = {fw: med(scen, size, 'pinned', fw) for fw in FRAMEWORKS}
        lead, lead_p = leader(free, scen), leader(pinned, scen)
        cells = []
        for fw in FRAMEWORKS:
            f, p = free[fw], pinned[fw]
            ratio = f'{f / p:.2f}x' if f and p else '-'
            cells.append(f'{fw} {f if f is not None else "n/a"} (pinned {p if p is not None else "n/a"}, '
                         f'free/pinned {ratio})')
        flag = '' if lead == lead_p else f'  LEAD CHANGES: pinned {lead_p} -> free {lead}'
        print(f'  {size}: lead {lead}{flag}\n    ' + '\n    '.join(cells))
        # Cost and delivery, free arm, over exactly the reps the headline counted.
        for fw in FRAMEWORKS:
            rs = counted(scen, size, 'free', fw)
            if not rs:
                continue
            figs = [s6_reps.figures(r, scen) for r in rs]
            print(f'      {fw} cost/delivery n={len(rs)}: ' + ' '.join(s6_reps.summarise(figs, scen)))
            hist = sorted({f['history'] for f in figs if 'history' in f})
            if hist:
                print(f'        history (arg client/server): {", ".join(hist)}')
