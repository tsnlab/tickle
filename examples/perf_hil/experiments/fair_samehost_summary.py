#!/usr/bin/env python3
"""Reads fair_samehost_remeasure.sh's output and prints the free (unpinned) headline beside the pinned note, then what
each framework's processes cost and what they delivered.

Usage: fair_samehost_summary.py <OUTB>

Rules, as in the published table and fixed before the run:
  - only each framework's ON arm (its own shared-memory transport) is read, and only reps s6_transport_cells.sh
    printed a RESULT for;
  - throughput is GOODPUT (the user, 2026-10-17; TESTING.md section 4): the server's win_recv_mbps, the complete
    samples its application took inside the measured window. The client's send rate (win_send_mbps, or send_mbps on
    a line from before the window) is printed beside it as a diagnostic and decides nothing. A rep with no server
    line, or whose server or client window= is not ok, has no rate. Latency is rtt_avg_ms. Medians of the reps;
  - every rep counts, including a BEST_EFFORT rep that dropped samples: goodput already prices a drop at zero. Until
    2026-10-17 a TickLE BEST_EFFORT rep with shm_full_dropped > 0 was excluded, because a dropping rep read a HIGHER
    send rate; the user retired that drop-free rule with goodput;
  - incomplete delivery in a RELIABLE cell (reliable_throughput: server recv < client sent; reliable_latency: client
    recv < sent, a round trip not answered), in any counted rep, follows the 2026-09-26 rule as campaign_summary.py's
    delivery_verdict() applies it cross-host: a vendor is listed as DELIVERY FAILED with its counts and excluded from
    the cell's leader and worst; an incomplete TickLE makes the cell a LOSE. BEST_EFFORT cells are scored on goodput
    alone (2026-10-17). Until 2026-10-17 this reader applied no delivery check at all;
  - a row's leader is decided on the free arm alone; a cell with fewer than 2 usable reps for a framework reads n/a.

Cost and delivery (2026-10-06, ROADMAP "Fill COMPARISON 1a's empty cells"): for the free arm, per framework, the
medians s6_reps.py defines - client, server and (CycloneDDS) iox-roudi CPU per sample, their sum per DELIVERED sample,
peak RSS of each and their sum, and on the throughput cells the server's delivered rate (win_recv_mbps) beside the
client's send rate. Over the same reps as the headline. Since 2026-10-17 the headline is the delivered rate itself
(goodput, above); the send rate stays in this block as a diagnostic.

Every line is read from <OUTB>.<scenario>_<size>_<arm>.txt, where s6_transport_cells.sh copies the TickLE cell's
lines (indented) beside the vendors'; the .txt.tickle files hold the same TickLE lines and are not read again. <size>
may carry the cell's own ping interval (p2_i0.05: fair_samehost_remeasure.sh's reliable_latency:p2:0.05:60, S8/S12).

LATENCY FROM CREATION TIME (the user's decision of 2026-10-17; TESTING.md section 4), the "creation" block at the end,
written 2026-10-17 before its first run and enforced in creation_block():
  C1 SAMPLES FIRST. Per rep of the free arm the headline counts: the client's measured round trips, crt_kept and
     crt_no_due are printed before any value. A rep without crt_* fields, with crt_no_due > 0, or with crt_kept !=
     measured is not read (its client did not record every measured ping's due time: an instrument fault, never a
     gap of 0). A mean or p50 is read with >= 50 kept round trips per rep, a p99 with >= 500; a framework with fewer
     than 2 readable reps is NOT READ.
  C2 CONTROL, creation_latency_summary.py's R4 (rtt_r4) per framework and cell: nothing waits at a ping-pong sender,
     so the creation and stamp p50s differ by the sender's timer wake g: PASS when g <= 0.2 ms, no gap below
     -0.02 ms and |d - g| <= the reps' stamp-p50 range + creation-p50 range + 1 us (the clients print p50 to 1 us;
     the creation range since 2026-10-18, rtt_r4's docstring). The control needs
     TickLE and at least one vendor read; a failed control makes the cell's creation verdicts NOT READ.
  C3 SCORING. S7-S10 are means: the primary figure is the creation mean (crt_avg_ms), median of the reps; TickLE is
     scored against EACH vendor separately by the range rule (TESTING.md section 4: WIN when every TickLE rep is below
     every rep of that vendor, LOSE when every vendor rep is below every TickLE rep, else DRAW). The creation p50 is
     scored the same way beside it, and the send-stamp mean (the old figure, diagnostic) too, so a verdict that moves
     with the definition shows. A vendor DELIVERY FAILED in the cell (the delivery rule above) is listed and not
     scored; TickLE DELIVERY FAILED makes the cell a LOSE.
"""
import glob
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s6_reps  # noqa: E402 - beside this file, found through the path set just above
from creation_latency_summary import range_verdict, rtt_r4  # noqa: E402 - beside this file, as s6_reps

outb = sys.argv[1]
SCENS = ('best_effort_throughput', 'reliable_throughput', 'reliable_latency')
FRAMEWORKS = ('tickle', 'fastdds', 'cyclonedds')
reps = {}  # (scen, size, arm) -> [rep]; size is the file's label: p2, or p2_i0.05 for a cell with its own interval
LABELS = {}  # scen -> the labels found, in order
for scen in SCENS:
    for arm in ('free', 'pinned'):
        for path in sorted(glob.glob(f'{glob.escape(outb)}.{scen}_*_{arm}.txt')):
            size = path[len(f'{outb}.{scen}_'):-len(f'_{arm}.txt')]
            reps[(scen, size, arm)] = s6_reps.read_reps(path)[0]
            LABELS.setdefault(scen, [])
            if size not in LABELS[scen]:
                LABELS[scen].append(size)
    LABELS[scen] = sorted(LABELS.get(scen, []))


def headline(rep, scen):
    """The published figure of one rep, or None if the rep does not count."""
    c = rep['client']
    if scen == 'reliable_latency':
        if (s6_reps.num(c, 'recv') or 0) == 0:
            return None
        return s6_reps.num(c, 'rtt_avg_ms')
    s = rep['server']
    if s is None or not s6_reps.window_ok(c) or not s6_reps.window_ok(s):
        return None
    return s6_reps.num(s, 'win_recv_mbps') if 'win_recv_mbps' in s else s6_reps.num(s, 'recv_mbps')


def shortfall(rep, scen):
    """(sent, delivered) when a RELIABLE rep delivered less than it sent, else None. BEST_EFFORT is never short."""
    if scen == 'best_effort_throughput':
        return None
    c = rep['client']
    sent = s6_reps.num(c, 'sent')
    got = s6_reps.num(c, 'recv') if scen == 'reliable_latency' else s6_reps.num(rep['server'], 'recv')
    if sent is None or got is None or got >= sent:
        return None
    return sent, got


def counted(scen, size, arm, fw):
    """The ON-arm reps of one framework that the headline counts."""
    return [r for r in reps.get((scen, size, arm), [])
            if r['fw'] == fw and r['arm'] == 'ON' and headline(r, scen) is not None]


def med(scen, size, arm, fw):
    v = [headline(r, scen) for r in counted(scen, size, arm, fw)]
    return st.median(v) if len(v) >= 2 else None


def delivery_failed(scen, size, arm):
    """{fw: [(sent, delivered), ...]} over the counted reps that fell short (RELIABLE cells only)."""
    out = {}
    for fw in FRAMEWORKS:
        short = [x for x in (shortfall(r, scen) for r in counted(scen, size, arm, fw)) if x is not None]
        if short:
            out[fw] = short
    return out


def leader(values, scen, failed=()):
    """(best, worst) among the frameworks that delivered everything; TickLE failing makes the cell a LOSE."""
    if 'tickle' in failed:
        return 'LOSE (TickLE delivery failed)', '-'
    have = {fw: v for fw, v in values.items() if v is not None and fw not in failed}
    if not have:
        return 'n/a', 'n/a'
    lo, hi = min(have, key=have.get), max(have, key=have.get)
    return (lo, hi) if scen == 'reliable_latency' else (hi, lo)


for scen in SCENS:
    unit = 'ms RTT, lower is better' if scen == 'reliable_latency' else 'goodput Mbps, higher is better'
    print(f'\n{scen} ({unit})')
    for size in LABELS.get(scen, []):
        free = {fw: med(scen, size, 'free', fw) for fw in FRAMEWORKS}
        pinned = {fw: med(scen, size, 'pinned', fw) for fw in FRAMEWORKS}
        failed, failed_p = delivery_failed(scen, size, 'free'), delivery_failed(scen, size, 'pinned')
        (lead, worst), (lead_p, _) = leader(free, scen, failed), leader(pinned, scen, failed_p)
        cells = []
        for fw in FRAMEWORKS:
            f, p = free[fw], pinned[fw]
            ratio = f'{f / p:.2f}x' if f and p else '-'
            cells.append(f'{fw} {f if f is not None else "n/a"} (pinned {p if p is not None else "n/a"}, '
                         f'free/pinned {ratio})')
        flag = '' if lead == lead_p else f'  LEAD CHANGES: pinned {lead_p} -> free {lead}'
        print(f'  {size}: lead {lead}, worst {worst}{flag}\n    ' + '\n    '.join(cells))
        for arm, fails in (('free', failed), ('pinned', failed_p)):
            for fw, short in fails.items():
                what = ', '.join(f'{got:.0f} of {sent:.0f}' for sent, got in short)
                print(f'    DELIVERY FAILED ({arm}): {fw} delivered {what} in {len(short)} of '
                      f'{len(counted(scen, size, arm, fw))} reps'
                      + (' - excluded from this cell' if fw != 'tickle' else ' - the cell is a LOSE'))
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

N_MED, N_P99 = 50, 500  # creation_latency_summary.py R1


def creation_reps(scen, size, fw):
    """C1: (readable client lines, lines printed for every counted rep)."""
    good, lines = [], []
    for r in counted(scen, size, 'free', fw):
        c = r['client']
        measured, kept, no_due = (s6_reps.num(c, k) for k in ('measured', 'crt_kept', 'crt_no_due'))
        why = []
        if kept is None or 'crt_avg_ms' not in c:
            why.append('no crt_* fields')
        elif no_due or kept != measured:
            why.append(f'crt_kept {kept:.0f} != measured {measured} or crt_no_due {no_due}')
        elif kept < N_MED:
            why.append(f'{kept:.0f} round trips < {N_MED}')
        lines.append(f'    {fw:<10} measured {measured} crt_kept {kept} crt_no_due {no_due}'
                     + (f'  NOT READ: {"; ".join(why)}' if why else ''))
        if not why:
            good.append(c)
    return good, lines


def creation_block():
    print('\n=== creation (latency from each ping\'s due time; free arm; C1-C3 in the docstring) ===')
    for size in LABELS.get('reliable_latency', []):
        scen = 'reliable_latency'
        failed = delivery_failed(scen, size, 'free')
        print(f'\n{scen} {size}:')
        per, ctl_ok, read = {}, True, []
        for fw in FRAMEWORKS:
            good, lines = creation_reps(scen, size, fw)
            print('\n'.join(lines))
            if len(good) < 2:
                print(f'    {fw}: NOT READ ({len(good)} readable reps)')
                if fw == 'tickle':
                    ctl_ok = False
                continue
            v = {k: [s6_reps.num(c, k) for c in good]
                 for k in ('crt_avg_ms', 'crt_p50_ms', 'crt_p99_ms', 'rtt_avg_ms', 'rtt_p50_ms', 'gap_p50_us')}
            gmin = min(s6_reps.num(c, 'gap_min_us') for c in good) / 1e3
            ok, text = rtt_r4(v['rtt_p50_ms'], v['crt_p50_ms'], [g / 1e3 for g in v['gap_p50_us']], gmin)
            p99 = (f"{st.median(v['crt_p99_ms']):.4f}" if all(s6_reps.num(c, 'crt_kept') >= N_P99 for c in good)
                   else f'NOT READ (< {N_P99} round trips)')
            print(f'    {fw:<10} creation mean {st.median(v["crt_avg_ms"]):.4f} [{min(v["crt_avg_ms"]):.4f}-'
                  f'{max(v["crt_avg_ms"]):.4f}] p50 {st.median(v["crt_p50_ms"]):.4f} p99 {p99} ms | stamp mean '
                  f'{st.median(v["rtt_avg_ms"]):.4f} [{min(v["rtt_avg_ms"]):.4f}-{max(v["rtt_avg_ms"]):.4f}] p50 '
                  f'{st.median(v["rtt_p50_ms"]):.4f} ms | gap p50 {st.median(v["gap_p50_us"]):.1f} us')
            print(f'    {fw:<10} control (R4): {text}')
            ctl_ok = ctl_ok and ok
            read.append(fw)
            per[fw] = v
        if 'tickle' not in read or len(read) < 2:
            ctl_ok = False
        print(f'  CONTROL {"PASS" if ctl_ok else "FAIL"}')
        if 'tickle' in failed:
            print('  verdicts: LOSE (TickLE delivery failed)')
            continue
        for vendor in ('fastdds', 'cyclonedds'):
            if vendor in failed:
                print(f'  vs {vendor}: DELIVERY FAILED, not scored')
                continue
            if 'tickle' not in per or vendor not in per:
                print(f'  vs {vendor}: NOT READ')
                continue
            t, o = per['tickle'], per[vendor]
            crt = (range_verdict(t['crt_avg_ms'], o['crt_avg_ms']), range_verdict(t['crt_p50_ms'], o['crt_p50_ms']))
            if not ctl_ok:
                crt = ('NOT READ (control failed)',) * 2
            print(f'  vs {vendor}: creation mean {crt[0]}, creation p50 {crt[1]}; stamp mean (diagnostic) '
                  f'{range_verdict(t["rtt_avg_ms"], o["rtt_avg_ms"])}')


creation_block()
