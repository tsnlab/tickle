#!/usr/bin/env python3
# stepA_kernel_read.py <OUT.d> - the reading of stepA_kernel_rig.sh, written 2026-10-11 before any of its runs. The
# rules are the harness header's, repeated here because this file is what applies them:
#
#   VOID run: a leftover bench process (its .note); no client RESULT; sent = 0; x: drained != acked or tx_shm != 0; s:
#     recv != sent or tx_shm/sent < 0.95; perf stat missing or not counted; snapshot pre or post missing; a pinned run
#     whose client left its CPU (cpu_main != pin or cpu_main_share < 0.99); an off run with >= 5% of the run's eth0
#     interrupts on its CPU, an on run with < 90%, or a pinned run that saw no eth0 interrupt at all.
#   TREATMENT: per variant A's and B's client sha256 differ, else the cell is VOID.
#   DIFFERS per (cell, placement, role, metric): |B - A| > 2 SE and > FLOOR (|mean A block 1 - mean A block 4|) and
#     >= min effect (instructions 0.3%, cycles 0.5%, a kernel event count 1% of A's mean and >= 0.002 per sample).
#     A = blocks 1+4, B = blocks 2+3.
#   x_p2, D_p = B - A client instructions:k per sample in placement p:
#     K0 D_free does not DIFFER with B higher -> run 1's +270 does not reproduce: no kernel finding (the gap is R4).
#     K1 D_free DIFFERS (B higher), D_off does not DIFFER and D_off < D_free / 2, and the POSITIVE CONTROL passes (A on -
#        A off instructions:k > 2 SE and >= 1% of A off) -> interrupt work charged to a publisher on-CPU longer.
#     K2 D_off DIFFERS (B higher) and D_off >= D_free / 2 -> more kernel work inside B's own calls; named by the
#        system-wide counters that DIFFER (B higher), or "unnamed by these counters".
#     K? anything else -> inconclusive, values printed.
#   s_p2: S0 client instructions:k does not DIFFER (B higher) -> run 2's +780 was noise. S1 it DIFFERS and an IPI or
#     softirq count per round trip DIFFERS (B higher) -> the wake path differs, named. S2 it DIFFERS, none does -> unnamed.
import collections
import glob
import math
import os
import re
import statistics as st
import sys

D = sys.argv[1]
VARIANT = {'x_p2': 'reliable_throughput_p2', 's_p2': 'reliable_latency_p2'}
STAT_KEYS = ['instructions:u', 'instructions:k', 'cycles:u', 'cycles:k', 'context-switches', 'cpu-migrations',
             'page-faults']

pin_cpu = {}
try:
    for kv in open(os.path.join(D, 'placement.txt')).read().split():
        k, v = kv.split('=')
        pin_cpu['on' if k == 'IRQ_CPU' else 'off'] = None if v == 'none' else int(v)
except OSError:
    pass

built = collections.defaultdict(dict)
for f in glob.glob(os.path.join(D, 'build_*_cli.txt')):
    for line in open(f, errors='replace'):
        m = re.match(r'BUILT (\S+) (\S+) (\S+) client=(\S+)', line)
        if m:
            built[m.group(1)][m.group(2)] = m.group(4)
void_cell = {}
for cell, v in VARIANT.items():
    a, b = built['A'].get(v), built['B'].get(v)
    if a is None or b is None:
        void_cell[cell] = f'TREATMENT could not look: no BUILT line for {v} (A {a}, B {b})'
    elif a == b:
        void_cell[cell] = f'TREATMENT failed: A and B built the same client {a} for {v}'
    print(f'TREATMENT {cell}: A client {a}  B client {b}  {"VOID: " + void_cell[cell] if cell in void_cell else "ok"}')


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
                    out[p[2]] = None
    except OSError:
        return None
    return out or None


def snapshot(path):
    """Counters of one snapshot: name -> value, plus 'eth0@<cpu>' per CPU. None if the file is incomplete."""
    try:
        text = open(path, errors='replace').read()
    except OSError:
        return None
    if '#END' not in text:
        return None
    out = {}
    sec = None
    ncpu = 0
    udp_hdr = None
    for line in text.splitlines():
        if line.startswith('#'):
            sec = line[1:].split()[0]
            continue
        t = line.split()
        if not t:
            continue
        if sec in ('INTERRUPTS', 'SOFTIRQS') and t[0].startswith('CPU'):
            ncpu = len(t)
            continue
        if sec == 'INTERRUPTS':
            nums = []
            for x in t[1:1 + ncpu]:
                if not x.isdigit():
                    break
                nums.append(int(x))
            name = ' '.join(t[1 + len(nums):])
            if 'eth0' in name:
                out['eth0_irq'] = out.get('eth0_irq', 0) + sum(nums)
                for i, n in enumerate(nums):
                    out[f'eth0@{i}'] = out.get(f'eth0@{i}', 0) + n
            elif t[0].startswith('IPI'):
                out['ipi:' + name] = sum(nums)
            elif 'arch_timer' in name:
                out['timer_irq'] = out.get('timer_irq', 0) + sum(nums)
        elif sec == 'SOFTIRQS':
            out['softirq:' + t[0].rstrip(':')] = sum(int(x) for x in t[1:] if x.isdigit())
        elif sec == 'STAT' and t[0] == 'cpu':
            for i, k in enumerate(['user', 'nice', 'system', 'idle', 'iowait', 'irq', 'softirq']):
                if k != 'idle' and k != 'nice' and len(t) > i + 1:
                    out['jiffies:' + k] = int(t[i + 1])
        elif sec == 'SNMP':
            if udp_hdr is None:
                udp_hdr = t[1:]
            else:
                for k, v in zip(udp_hdr, t[1:]):
                    out['udp:' + k] = int(v)
        elif sec == 'SOFTNET':
            try:
                c = [int(x, 16) for x in t]
            except ValueError:
                continue
            out['softnet:processed'] = out.get('softnet:processed', 0) + c[0]
            out['softnet:dropped'] = out.get('softnet:dropped', 0) + c[1]
            out['softnet:time_squeeze'] = out.get('softnet:time_squeeze', 0) + c[2]
        elif sec == 'QDISC' and t[0] == 'Sent' and 'qdisc:pkt' not in out:  # the root qdisc's line comes first
            m = re.search(r'Sent (\d+) bytes (\d+) pkt \(dropped (\d+), overlimits (\d+) requeues (\d+)', line)
            if m:
                out['qdisc:pkt'] = int(m.group(2))
                out['qdisc:dropped'] = int(m.group(3))
                out['qdisc:requeues'] = int(m.group(5))
    return out


vals = collections.defaultdict(lambda: collections.defaultdict(lambda: collections.defaultdict(list)))
voids = []
treat = collections.defaultdict(list)  # (cell, pl, arm) -> realized placement lines
for f in sorted(glob.glob(os.path.join(D, 'b*_*_*_*_r*.client.log'))):
    m = re.match(r'b(\d)_([AB])_(x_p2|s_p2)_(free|off|on)_r(\d+)\.client\.log$', os.path.basename(f))
    if not m:
        continue
    blk, arm, cell, pl = int(m.group(1)), m.group(2), m.group(3), m.group(4)
    base = f[:-len('.client.log')]
    tag = os.path.basename(base)
    if cell in void_cell:
        continue
    try:
        note = open(base + '.note', errors='replace').read()
    except OSError:
        note = ''
    if 'leftover' in note:
        voids.append(f'{tag}: {note.strip()}')
        continue
    r = result(f)
    if r is None:
        voids.append(f'{tag}: no client RESULT line')
        continue
    sent = int(r.get('sent', 0))
    if sent == 0:
        voids.append(f'{tag}: sent=0')
        continue
    if cell == 'x_p2':
        if r.get('drained') != 'acked' or int(r.get('tx_shm', 0)) != 0:
            voids.append(f'{tag}: drained={r.get("drained")} tx_shm={r.get("tx_shm")} (not a clean cross-host run)')
            continue
    elif int(r.get('recv', -1)) != sent or int(r.get('tx_shm', 0)) / sent < 0.95:
        voids.append(f'{tag}: recv={r.get("recv")} sent={sent} tx_shm={r.get("tx_shm")} (not a same-host segment run)')
        continue
    pre, post = snapshot(base + '.pre.txt'), snapshot(base + '.post.txt')
    if pre is None or post is None:
        voids.append(f'{tag}: snapshot pre or post missing or incomplete')
        continue
    delta = {k: post[k] - pre[k] for k in post if k in pre}
    if pl in ('off', 'on'):
        cpu = pin_cpu.get(pl)
        eth = delta.get('eth0_irq', 0)
        on_pin = delta.get(f'eth0@{cpu}', 0) / eth if eth else float('nan')
        share = float(r.get('cpu_main_share', 0))
        line = f'{tag}: pin {cpu} cpu_main {r.get("cpu_main")} share {share:.2f} eth0 irqs {eth} on pin {on_pin:.3f}'
        treat[(cell, pl, arm)].append(line)
        bad = None
        if cpu is None or str(r.get('cpu_main')) != str(cpu) or share < 0.99:
            bad = 'the client did not stay on its CPU'
        elif not eth:
            bad = 'no eth0 interrupt seen during the run (could not look)'
        elif pl == 'off' and on_pin >= 0.05:
            bad = 'the off CPU took >= 5% of eth0 interrupts'
        elif pl == 'on' and on_pin < 0.90:
            bad = 'the on CPU took < 90% of eth0 interrupts (the IRQ moved)'
        if bad:
            voids.append(f'{line} - {bad}')
            continue
    roles = ['client'] + (['server'] if cell == 's_p2' else [])
    ok = True
    for role in roles:
        s = perf_stat(f'{base}.{role}.perf')
        if s is None or any(s.get(k) is None for k in STAT_KEYS[:4]):
            voids.append(f'{tag}: {role} perf stat missing or not counted')
            ok = False
            break
        for k in STAT_KEYS:
            if s.get(k) is not None:
                vals[(cell, pl, role, k)][arm][blk].append(s[k] / sent)
    if not ok:
        continue
    for k in ('cpu_s_per_Msample', 'rtt_p50_ms'):
        if k in r:
            vals[(cell, pl, 'client', k)][arm][blk].append(float(r[k]))
    for k, v in delta.items():
        if '@' not in k:
            vals[(cell, pl, 'system', k)][arm][blk].append(v / sent)

for v in voids:
    print('VOID', v)
for k, lines in sorted(treat.items()):
    print(f'PLACEMENT {k[0]} {k[1]} {k[2]}:')
    for line in lines:
        print('   ', line)


def min_effect(metric, a_mean):
    if metric.startswith('instructions'):
        return 0.003 * abs(a_mean)
    if metric.startswith('cycles'):
        return 0.005 * abs(a_mean)
    if metric.startswith('jiffies:') or metric in STAT_KEYS or metric in ('cpu_s_per_Msample', 'rtt_p50_ms'):
        return 0.01 * abs(a_mean)
    # A kernel event count: 1% of A's, and at least one event per 500 samples - fewer could not explain a kernel
    # instruction gap of hundreds per sample (it would have to cost > 100k instructions), and among some 40 counters a
    # few tiny ones pass 2 SE by chance.
    return max(0.01 * abs(a_mean), 0.002)


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
    differs = abs(d) > 2 * se and abs(d) > floor and abs(d) >= min_effect(key[3], ma)
    return dict(a=ma, b=mb, d=d, se=se, floor=floor, differs=differs, na=len(a), nb=len(b),
                pct=(d / ma * 100) if ma else float('nan'))


def show(key, c, mark=''):
    tag = 'DIFFERS' if c['differs'] else 'same   '
    print(f'  {tag} {key[0]:4} {key[1]:4} {key[2]:6} {key[3]:30} A {c["a"]:12.4f} (n{c["na"]})  B {c["b"]:12.4f} '
          f'(n{c["nb"]})  B-A {c["d"]:+10.4f} ({c["pct"]:+6.2f}%)  2SE {2 * c["se"]:.4f}  floor {c["floor"]:.4f}{mark}')


def table(cell, pl):
    out = {}
    for key in sorted(k for k in vals if k[0] == cell and k[1] == pl):
        c = compare(key)
        if c is None:
            continue
        if key[2] == 'system' and not c['differs'] and max(abs(c['a']), abs(c['b'])) < 1e-4:
            continue  # a counter that hardly moves: not printed unless it differs
        show(key, c, '   <- D' if key[2] == 'client' and key[3] == 'instructions:k' else '')
        out[key] = c
    return out


def higher(c):
    return c is not None and c['differs'] and c['d'] > 0


# ------------------------------------------------------------------------------------------------ x_p2
res = {}
print('\n=== x_p2 (reliable_throughput_p2, client = publisher) ===')
if 'x_p2' in void_cell:
    print('  VOID:', void_cell['x_p2'])
else:
    for pl in ('free', 'off', 'on'):
        print(f'  --- placement {pl}' + (f' (cpu {pin_cpu.get(pl)})' if pl != 'free' else '') + ' ---')
        res[pl] = table('x_p2', pl)
    DK = {pl: res.get(pl, {}).get(('x_p2', pl, 'client', 'instructions:k')) for pl in ('free', 'off', 'on')}
    pc = None
    pon, poff = vals.get(('x_p2', 'on', 'client', 'instructions:k')), vals.get(('x_p2', 'off', 'client',
                                                                                 'instructions:k'))
    if pon and poff:
        a_on, a_off = pon['A'][1] + pon['A'][4], poff['A'][1] + poff['A'][4]
        if len(a_on) >= 2 and len(a_off) >= 2:
            d = st.mean(a_on) - st.mean(a_off)
            se = math.sqrt(st.variance(a_on) / len(a_on) + st.variance(a_off) / len(a_off))
            pc = d > 2 * se and d >= 0.01 * st.mean(a_off)
            print(f'  POSITIVE CONTROL (A on - A off instructions:k/sample): {d:+.1f} 2SE {2 * se:.1f} '
                  f'({d / st.mean(a_off) * 100:+.2f}%) -> {"passes" if pc else "FAILS: interrupt work charged to the task is not visible"}')
    if pc is None:
        print('  POSITIVE CONTROL: could not look (too few valid on/off runs)')
    df, do = DK["free"], DK["off"]
    if df is None:
        reading = 'could not look (too few valid free runs)'
    elif not higher(df):
        reading = 'K0 run 1\'s +270 instructions:k does not reproduce: no kernel finding; the step A gap is cycles at equal work (R4)'
    elif do is not None and not higher(do) and do['d'] < df['d'] / 2 and pc:
        sysdiff = [k[3] for k, c in res['free'].items() if k[2] == 'system' and c['differs']]
        reading = ('K1 interrupt work charged to a publisher that is on-CPU longer: B asks the kernel for nothing more'
                   f' (system-wide counters that DIFFER in free: {", ".join(sysdiff) or "none"})')
    elif higher(do) and do['d'] >= df['d'] / 2:
        named = [k[3] for k, c in res['off'].items() if k[2] == 'system' and higher(c)]
        reading = ('K2 more kernel work inside B\'s own calls: ' +
                   (', '.join(named) if named else 'unnamed by these counters'))
    else:
        reading = (f'K? inconclusive: D_free {df["d"]:+.1f}, D_off ' + (f'{do["d"]:+.1f}' if do else 'n/a') +
                   f', positive control {pc}')
    print('  READING x_p2:', reading)
    for pl in ('free', 'off', 'on'):
        c = res.get(pl, {}).get(('x_p2', pl, 'client', 'cycles:u'))
        if c:
            print(f'  cycles:u gap, placement {pl}: {c["d"]:+.1f} ({c["pct"]:+.2f}%) {"DIFFERS" if c["differs"] else "same"}')

# ------------------------------------------------------------------------------------------------ s_p2
print('\n=== s_p2 (reliable_latency_p2, same host) ===')
if 's_p2' in void_cell:
    print('  VOID:', void_cell['s_p2'])
else:
    t = table('s_p2', 'free')
    c = t.get(('s_p2', 'free', 'client', 'instructions:k'))
    if c is None:
        reading = 'could not look (too few valid runs)'
    elif not higher(c):
        reading = 'S0 run 2\'s +780 client instructions:k does not reproduce: noise at n4'
    else:
        named = [k[3] for k, v in t.items() if k[2] == 'system' and higher(v) and
                 (k[3].startswith('ipi:') or k[3].startswith('softirq:'))]
        reading = ('S1 the wake path differs: ' + ', '.join(named)) if named else 'S2 more kernel work, unnamed'
    print('  READING s_p2:', reading)
