#!/usr/bin/env python3
# stepA_perf_read.py <OUT.d> - the reading of stepA_perf_rig.sh, written 2026-10-10 before any of its runs. The rules
# are the harness header's, repeated here because this file is what applies them:
#
#   VOID run: a bench process left over from before it (its .note); no client RESULT line; sent = 0; x cells: drained != acked or tx_shm != 0; s cells: recv != sent or
#     tx_shm/sent < 0.95; a mode's instrument output missing, empty or holding "<not counted>"/"<not supported>" for an
#     event the reading uses.
#   TREATMENT: per variant, A's and B's client sha256 (the builds' BUILT lines) differ - otherwise the cell is VOID.
#   CONTROL: blocks 1 and 4 are both A. FLOOR(metric) = |mean(A, block 1) - mean(A, block 4)|.
#   DIFFERS: |B - A| > 2 x SE(B - A) and |B - A| > FLOOR and |B - A| >= the metric's smallest effect (min_effect():
#     instructions 0.3%, cycles 0.5%, counts 0.01 per sample, a symbol 0.5% of its role's cycles). A = blocks 1+4,
#     B = blocks 2+3; SE over runs. R1-R4 count a metric only when B is the higher: only more can explain slower.
#   REPRODUCED: the plain mode's primary (x: client cpu_s_per_Msample; s: client rtt_p50_ms) DIFFERS with B higher.
#   Reading of a reproduced cell, first match wins: R1 instructions:u/sample DIFFERS (client, or either role on s) ->
#     user work; R2 any syscall/sample DIFFERS (strace by name, or the raw_syscalls tracepoint) -> a system call; R3
#     instructions:k DIFFERS -> kernel work per call; R4 cycles:u or cycles:k DIFFERS with u/k instructions and syscalls
#     equal -> the same work slower (placement / microarchitecture); R5 none -> could not look.
#   "placement, not work" is FALSIFIED by R1, R2 or R3 on any reproduced cell.
import collections
import glob
import math
import os
import re
import statistics as st
import sys

D = sys.argv[1]
CELLS = ['x_p2', 'x_p3', 's_p2']
VARIANT = {'x_p2': 'reliable_throughput_p2', 'x_p3': 'reliable_throughput_p3', 's_p2': 'reliable_latency_p2'}
STAT_KEYS = ['instructions:u', 'instructions:k', 'cycles:u', 'cycles:k', 'context-switches', 'cpu-migrations',
             'page-faults', 'raw_syscalls:sys_enter']

# ------------------------------------------------------------------------------------------------ treatment
built = collections.defaultdict(dict)  # arm -> variant -> client sha (from the client Pi's build)
for f in glob.glob(os.path.join(D, 'build_*_cli.txt')):
    for line in open(f, errors='replace'):
        m = re.match(r'BUILT (\S+) (\S+) (\S+) client=(\S+)', line)
        if m:
            built[m.group(1)][m.group(2)] = m.group(4)
void_cell = {}
for cell in CELLS:
    v = VARIANT[cell]
    a, b = built['A'].get(v), built['B'].get(v)
    if a is None or b is None:
        void_cell[cell] = f'TREATMENT could not look: no BUILT line for {v} (A {a}, B {b})'
    elif a == b:
        void_cell[cell] = f'TREATMENT failed: A and B built the same client {a} for {v}'
    print(f'TREATMENT {cell}: A client {a}  B client {b}  {"VOID: " + void_cell[cell] if cell in void_cell else "ok"}')


# ------------------------------------------------------------------------------------------------ runs
def result(path):
    try:
        for line in open(path, errors='replace'):
            if line.startswith('RESULT'):
                return dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    except OSError:
        pass
    return None


def perf_stat(path):
    out = {}
    try:
        for line in open(path, errors='replace'):
            p = line.strip().split(',')
            if len(p) > 3 and not line.startswith('#'):
                try:
                    out[p[2]] = float(p[0])
                except ValueError:
                    out[p[2]] = None  # <not counted> / <not supported>
    except OSError:
        return None
    return out or None


def strace_c(path):
    out = {}
    try:
        for line in open(path, errors='replace'):
            p = line.split()
            if len(p) >= 5 and p[-1] not in ('syscall', 'total') and re.match(r'^[\d.]+$', p[0]):
                out[p[-1]] = out.get(p[-1], 0) + int(p[3])
    except OSError:
        return None
    return out or None


def report(path):
    rows, fallback = {}, False
    try:
        for line in open(path, errors='replace'):
            if line.startswith('#FALLBACK'):
                fallback = True
                continue
            if line.startswith('#') or not line.strip():
                continue
            # An address perf could not name (kernel symbols hidden from this user, a stripped libc) is summed per dso:
            # one line per instruction address would split one function's cycles across hundreds of rows.
            if not fallback:
                p = [x.strip() for x in line.split(';')]
                if len(p) >= 4:
                    try:
                        sym = p[3].replace('[.] ', '').replace('[k] ', '')
                        key = (p[2], '[unresolved]' if sym.startswith('0x') else sym)
                        rows[key] = rows.get(key, 0.0) + float(p[0])
                    except ValueError:
                        pass
            else:
                m = re.match(r'\s*([\d.]+)%\s+(\S+)\s+\[[.k]\]\s+(\S+)', line)
                if m:
                    key = (m.group(2), '[unresolved]' if m.group(3).startswith('0x') else m.group(3))
                    rows[key] = rows.get(key, 0.0) + float(m.group(1))
    except OSError:
        return None, False
    return (rows or None), fallback


# vals[(cell, role, metric)][arm][block] -> list of per-run values
vals = collections.defaultdict(lambda: collections.defaultdict(lambda: collections.defaultdict(list)))
voids = []
fallback_reports = collections.defaultdict(list)  # (cell, role, arm, block) -> share dicts, converted after stat
for f in sorted(glob.glob(os.path.join(D, 'b*_*_*_*_r*.client.log'))):
    m = re.match(r'b(\d)_([AB])_(x_p\d|s_p\d)_(plain|stat|record|strace)_r(\d+)\.client\.log$', os.path.basename(f))
    if not m:
        continue
    blk, arm, cell, mode, rep = int(m.group(1)), m.group(2), m.group(3), m.group(4), m.group(5)
    base = f[:-len('.client.log')]
    if cell in void_cell:
        continue
    r = result(f)
    tag = os.path.basename(base)
    try:
        note = open(base + '.note', errors='replace').read()
    except OSError:
        note = ''
    if 'leftover' in note:
        voids.append(f'{tag}: {note.strip()}')
        continue
    if r is None:
        voids.append(f'{tag}: no client RESULT line')
        continue
    sent = int(r.get('sent', 0))
    if sent == 0:
        voids.append(f'{tag}: sent=0')
        continue
    if cell.startswith('x_'):
        if r.get('drained') != 'acked' or int(r.get('tx_shm', 0)) != 0:
            voids.append(f'{tag}: drained={r.get("drained")} tx_shm={r.get("tx_shm")} (not a clean cross-host run)')
            continue
    else:
        if int(r.get('recv', -1)) != sent or int(r.get('tx_shm', 0)) / sent < 0.95:
            voids.append(f'{tag}: recv={r.get("recv")} sent={sent} tx_shm={r.get("tx_shm")} (not a same-host segment run)')
            continue
    roles = ['client'] + (['server'] if cell.startswith('s_') else [])
    if mode == 'plain':
        for k in ('cpu_s_per_Msample', 'rtt_p50_ms', 'rtt_avg_ms', 'send_mbps'):
            if k in r:
                vals[(cell, 'client', k)][arm][blk].append(float(r[k]))
    elif mode == 'stat':
        for role in roles:
            s = perf_stat(f'{base}.{role}.perf')
            if s is None or any(s.get(k) is None for k in STAT_KEYS[:4]):
                voids.append(f'{tag}: {role} perf stat missing or not counted')
                continue
            for k in STAT_KEYS:
                if s.get(k) is not None:
                    vals[(cell, role, k)][arm][blk].append(s[k] / sent)
    elif mode == 'strace':
        for role in roles:
            s = strace_c(f'{base}.{role}.strace')
            if s is None:
                voids.append(f'{tag}: {role} strace table missing')
                continue
            vals[(cell, role, 'syscalls_total')][arm][blk].append(sum(s.values()) / sent)
            for name, n in s.items():
                vals[(cell, role, 'sys:' + name)][arm][blk].append(n / sent)
    elif mode == 'record':
        for role in roles:
            rows, fb = report(f'{base}.{role}.report')
            if rows is None:
                voids.append(f'{tag}: {role} perf report empty')
                continue
            if fb:
                fallback_reports[(cell, role, arm, blk)].append(rows)
                continue
            for (dso, sym), period in rows.items():
                vals[(cell, role, f'cyc:{dso}:{sym}')][arm][blk].append(period / sent)
# A fallback report holds shares: converted with that arm/block's stat-mode cycles per sample (u + k).
for (cell, role, arm, blk), lst in fallback_reports.items():
    cu = vals[(cell, role, 'cycles:u')][arm][blk]
    ck = vals[(cell, role, 'cycles:k')][arm][blk]
    if not cu or not ck:
        voids.append(f'{cell} {role} {arm} b{blk}: fallback report with no stat cycles to convert it')
        continue
    total = st.mean(cu) + st.mean(ck)
    for rows in lst:
        for (dso, sym), pct in rows.items():
            vals[(cell, role, f'cyc:{dso}:{sym}')][arm][blk].append(pct / 100 * total)
# A symbol absent from a run's report had 0 cycles there: pad so means are over the same runs.
nrec = collections.Counter()
for f in glob.glob(os.path.join(D, 'b*_record_r*.client.report')):
    m = re.match(r'b(\d)_([AB])_(x_p\d|s_p\d)_record', os.path.basename(f))
    if m:
        nrec[(m.group(3), m.group(2), int(m.group(1)))] += 1
for key, per_arm in vals.items():
    if not key[2].startswith('cyc:'):
        continue
    for arm in 'AB':
        for blk in ((1, 4) if arm == 'A' else (2, 3)):
            want = nrec[(key[0], arm, blk)]
            have = per_arm[arm][blk]
            have.extend([0.0] * max(0, want - len(have)))

for v in voids:
    print('VOID', v)


# ------------------------------------------------------------------------------------------------ comparisons
# The smallest difference that counts, per metric (2026-10-11, before the first rig run, after the PC run of
# stepA_perf_pc.sh showed why: a count that is the same every run has SE 0, so a once-per-process call divided by a
# `sent` that varies by 0.1% read DIFFERS). Instructions 0.3% (largemsg_insn_ab.sh's A/A floor), cycles 0.5%, a count
# per sample 0.01 (one extra event per 100 samples), a symbol's cycles 0.5% of the role's own cycles per sample.
def min_effect(key, a_mean):
    m = key[2]
    if m.startswith('instructions'):
        return 0.003 * abs(a_mean)
    if m.startswith('cycles'):
        return 0.005 * abs(a_mean)
    if m.startswith('cyc:'):
        per = [vals.get((key[0], key[1], c)) for c in ('cycles:u', 'cycles:k')]
        tot = sum(st.mean(p['A'][1] + p['A'][4]) for p in per if p and (p['A'][1] + p['A'][4]))
        return 0.005 * tot if tot else 0.0
    if m.startswith('sys') or m in ('raw_syscalls:sys_enter', 'context-switches', 'cpu-migrations', 'page-faults'):
        return 0.01
    return 0.0


def compare(key):
    per = vals.get(key)
    if per is None:
        return None
    a = per['A'][1] + per['A'][4]
    b = per['B'][2] + per['B'][3]
    if len(a) < 2 or len(b) < 2:
        return None
    ma, mb = st.mean(a), st.mean(b)
    se = math.sqrt(st.variance(a) / len(a) + st.variance(b) / len(b))
    a1, a4 = per['A'][1], per['A'][4]
    floor = abs(st.mean(a1) - st.mean(a4)) if a1 and a4 else float('inf')
    d = mb - ma
    differs = abs(d) > 2 * se and abs(d) > floor and abs(d) >= min_effect(key, ma)
    return dict(a=ma, b=mb, d=d, se=se, floor=floor, differs=differs, na=len(a), nb=len(b),
                pct=(d / ma * 100) if ma else float('nan'))


def line(cell, role, metric, c):
    tag = 'DIFFERS' if c['differs'] else 'same   '
    return (f'  {tag} {cell:5} {role:6} {metric:34} A {c["a"]:12.4f} (n{c["na"]})  B {c["b"]:12.4f} (n{c["nb"]})'
            f'  B-A {c["d"]:+10.4f} ({c["pct"]:+6.2f}%)  2SE {2 * c["se"]:.4f}  floor {c["floor"]:.4f}')


falsified = []
for cell in CELLS:
    print(f'\n=== {cell} ({VARIANT[cell]}) ===')
    if cell in void_cell:
        print('  VOID:', void_cell[cell])
        continue
    prim = 'cpu_s_per_Msample' if cell.startswith('x_') else 'rtt_p50_ms'
    roles = ['client'] + (['server'] if cell.startswith('s_') else [])
    for metric in [prim] + (['cpu_s_per_Msample', 'rtt_avg_ms'] if cell.startswith('s_') else []):
        c = compare((cell, 'client', metric))
        if c:
            print(line(cell, 'client', metric, c) + ('   <- PRIMARY' if metric == prim else ''))
    p = compare((cell, 'client', prim))
    reproduced = bool(p and p['differs'] and p['d'] > 0)
    print(f'  REPRODUCED: {"yes" if reproduced else ("no" if p else "could not look (too few plain runs)")}')
    found = {}
    for role in roles:
        for metric in STAT_KEYS + ['syscalls_total']:
            c = compare((cell, role, metric))
            if c:
                print(line(cell, role, metric, c))
                found[(role, metric)] = c['differs'] and c['d'] > 0  # only more in B can explain B slower
        sysnames = sorted(k[2] for k in vals if k[0] == cell and k[1] == role and k[2].startswith('sys:'))
        for k in sysnames:
            c = compare((cell, role, k))
            if c and (c['differs'] or max(c['a'], c['b']) >= 0.01):
                print(line(cell, role, k, c))
            if c:
                found[(role, k)] = c['differs'] and c['d'] > 0

    def any_(pred):
        return any(v for (role, m), v in found.items() if pred(m))
    r1 = any_(lambda m: m == 'instructions:u')
    r2 = any_(lambda m: m.startswith('sys:') or m in ('syscalls_total', 'raw_syscalls:sys_enter'))
    r3 = any_(lambda m: m == 'instructions:k')
    r4 = any_(lambda m: m in ('cycles:u', 'cycles:k'))
    if not found:
        reading = 'could not look (no stat or strace run survived)'
    elif r1:
        reading = 'R1 more user work in B (instructions:u) - see the symbol table'
    elif r2:
        reading = 'R2 a system call per sample differs - named above'
    elif r3:
        reading = 'R3 more kernel work per call (instructions:k, syscalls equal)'
    elif r4:
        reading = 'R4 the same work done slower: placement / microarchitecture - the symbol table localises it'
    else:
        reading = 'R5 nothing the instruments see differs: could not look'
    print(f'  READING: {reading}' + ('' if reproduced else '   [gap NOT reproduced this session: claims nothing]'))
    if reproduced and (r1 or r2 or r3):
        falsified.append(cell)
    for role in roles:
        rows = []
        for k in vals:
            if k[0] == cell and k[1] == role and k[2].startswith('cyc:'):
                c = compare(k)
                if c:
                    rows.append((abs(c['d']), k[2][4:], c))
        rows.sort(reverse=True)
        if rows:
            print(f'  per-symbol cycles per sample, {role}, the 25 largest |B-A| (A blocks 1+4, B blocks 2+3):')
            for _, name, c in rows[:25]:
                only = ' [A only]' if c['b'] == 0 else (' [B only]' if c['a'] == 0 else '')
                print(f'    {"DIFFERS" if c["differs"] else "same   "} {name[:60]:60} A {c["a"]:9.2f}  B {c["b"]:9.2f}'
                      f'  B-A {c["d"]:+8.2f}  2SE {2 * c["se"]:.2f}{only}')
print()
print('"placement, not work":', ('FALSIFIED on ' + ', '.join(falsified)) if falsified else
      'not falsified (no reproduced cell shows more user work, a syscall or more kernel work)')
