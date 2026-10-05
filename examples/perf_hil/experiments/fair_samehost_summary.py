#!/usr/bin/env python3
"""Reads fair_samehost_remeasure.sh's output and prints the free (unpinned) headline beside the pinned note.

Usage: fair_samehost_summary.py <OUTB>

Rules, as in the published table and fixed before the run:
  - only each framework's ON arm (its own shared-memory transport) is read, and only reps s6_transport_cells.sh
    printed a RESULT for;
  - throughput is send_mbps, latency rtt_avg_ms, medians of the reps;
  - a TickLE BEST_EFFORT rep that dropped at a full ring (shm_full_dropped > 0) is excluded, as for S1-S3;
  - a row's leader is decided on the free arm alone; a cell with fewer than 2 usable reps for a framework reads n/a.
"""
import glob
import re
import statistics as st
import sys

outb = sys.argv[1]
rows = {}  # (scen, size, arm, framework) -> [values]
for path in glob.glob(f'{outb}.*_*_*.txt*'):
    if path.endswith('.log'):
        continue
    m = re.search(r'\.(best_effort_throughput|reliable_throughput|reliable_latency)_(p\d)_(free|pinned)\.txt', path)
    if not m:
        continue
    scen, size, arm = m.groups()
    for line in open(path, errors='replace'):
        r = re.match(r'arm=ON RESULT: framework=(\S+) scenario=\S+ (?!role=server)(.*)', line)
        if not r:
            continue
        f = dict(kv.split('=', 1) for kv in r.group(2).split() if '=' in kv)
        fw = r.group(1)
        if scen == 'reliable_latency':
            if int(f.get('recv', 0)) == 0:
                continue
            value = float(f['rtt_avg_ms'])
        else:
            if 'send_mbps' not in f:
                continue
            if fw == 'tickle' and scen == 'best_effort_throughput' and int(f.get('shm_full_dropped', 0)) > 0:
                continue
            value = float(f['send_mbps'])
        rows.setdefault((scen, size, arm, fw), []).append(value)

def med(key):
    v = rows.get(key, [])
    return st.median(v) if len(v) >= 2 else None

frameworks = ('tickle', 'fastdds', 'cyclonedds')
for scen in ('best_effort_throughput', 'reliable_throughput', 'reliable_latency'):
    unit = 'ms RTT, lower is better' if scen == 'reliable_latency' else 'send Mbps, higher is better'
    print(f'\n{scen} ({unit})')
    for size in ('p2', 'p3', 'p4'):
        free = {fw: med((scen, size, 'free', fw)) for fw in frameworks}
        pinned = {fw: med((scen, size, 'pinned', fw)) for fw in frameworks}
        have = {fw: v for fw, v in free.items() if v is not None}
        lead = (min(have, key=have.get) if scen == 'reliable_latency' else max(have, key=have.get)) if have else 'n/a'
        have_p = {fw: v for fw, v in pinned.items() if v is not None}
        lead_p = (min(have_p, key=have_p.get) if scen == 'reliable_latency' else max(have_p, key=have_p.get)) if have_p else 'n/a'
        cells = []
        for fw in frameworks:
            f, p = free[fw], pinned[fw]
            ratio = f'{f / p:.2f}x' if f and p else '-'
            cells.append(f'{fw} {f if f is not None else "n/a"} (pinned {p if p is not None else "n/a"}, free/pinned {ratio})')
        flag = '' if lead == lead_p else f'  LEAD CHANGES: pinned {lead_p} -> free {lead}'
        print(f'  {size}: lead {lead}{flag}\n    ' + '\n    '.join(cells))
