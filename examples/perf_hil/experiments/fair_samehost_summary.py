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
    for size in ('p2', 'p3', 'p4'):
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
