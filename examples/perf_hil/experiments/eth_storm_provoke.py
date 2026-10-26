#!/usr/bin/env python3
"""eth_storm_provoke.py - the EWS provocation (RESULTS `EWS`): make and read two experiment-only trees.

  make TREE BACKOFF   edit TREE/src/tickle.c (a copy of this checkout's, trace hooks included): the large repair
                      timeout pinned at 1465 x 1.024 us (~1.5 ms, the storm rep's value) wherever srtt is measured;
                      BACKOFF=1 also waits base << min(resends - 1, 6) before releasing a unit already resent (x2 a
                      resend, at most x64). Never production code.
  read OUT            the reading, fixed before the run:
      STORM (as eth_storm_read.py): the writer's retransmitted >= 100,000.
      P1 (pinned, no back-off): LOOP REACHABLE when >= 3 of its reps storm; NOT SUFFICIENT when 0 storm (a ~1.5 ms
         timeout alone does not start it: a trigger is needed); otherwise WEAK.
      P2 (pinned + back-off), read only when P1 is LOOP REACHABLE: BACK-OFF BREAKS IT when 0 of its reps storm and its
         median resends are within 2 x the census's calm median (3,136, ews1 round 1); otherwise NOT SHOWN.
"""
import glob
import re
import statistics
import sys

CALM_MEDIAN = 3136


def make(tree, backoff):
    p = tree + '/src/tickle.c'
    s = open(p).read()

    def rep(old, new):
        nonlocal s
        assert s.count(old) == 1, old[:70]
        s = s.replace(old, new)

    rep('''        order->timeout = (uint32_t)(((uint64_t)ack->repair_srtt_ns * 2U) >> LARGE_REPAIR_SHIFT) + 1U;''',
        '''        order->timeout = 1465U; // EWS provocation: the timeout pinned at ~1.5 ms, the storm rep's value''')
    if backoff:
        rep('''    if (now - slot->resent_at >= (order->timeout != 0 ? order->timeout : slot->hold)) {''',
            '''    uint32_t ews_base = order->timeout != 0 ? order->timeout : slot->hold;
    uint32_t ews_shift = slot->resends > 1U ? (uint32_t)(slot->resends - 1U) : 0U;
    ews_shift = ews_shift < 6U ? ews_shift : 6U;
    if (now - slot->resent_at >= (ews_base << ews_shift)) { // EWS provocation: per-unit back-off, x2 a resend''')
    open(p, 'w').write(s)


def arm(out, label):
    rows = []
    for run in sorted(glob.glob(f'{out}/{label}/runs/*')):
        try:
            pl = open(run + '/ping.log', errors='replace').read()
        except OSError:
            continue
        m = re.search(r'^RESULT: role=pub.*?retransmitted=(\d+)', pl, re.M)
        t = re.search(r'released_timeout=(\d+)', pl)
        if m:
            rows.append((int(m.group(1)), int(t.group(1)) if t else -1, run.rsplit('/', 1)[1]))
    return rows


def read(out):
    res = {}
    for label in ('P1', 'P2'):
        rows = arm(out, label)
        storms = [r for r in rows if r[0] >= 100_000]
        med = statistics.median(r[0] for r in rows) if rows else None
        res[label] = (rows, storms, med)
        print(f'{label}: {len(rows)} reps, STORM {len(storms)}, resends median {med}, per rep '
              f'{[(r[0], r[1]) for r in rows]}')
    rows, storms, _ = res['P1']
    p1 = 'LOOP REACHABLE' if len(storms) >= 3 else 'NOT SUFFICIENT' if not storms else 'WEAK'
    print(f'P1 {p1} ({len(storms)} of {len(rows)} stormed with the timeout pinned at ~1.5 ms)')
    if p1 == 'LOOP REACHABLE':
        rows, storms, med = res['P2']
        ok = rows and not storms and med <= 2 * CALM_MEDIAN
        print(f'P2 {"BACK-OFF BREAKS IT" if ok else "NOT SHOWN"} ({len(storms)} of {len(rows)} stormed, median '
              f'resends {med} against 2 x {CALM_MEDIAN})')
    else:
        print('P2 not read (P1 is not LOOP REACHABLE)')


if __name__ == '__main__':
    if sys.argv[1] == 'make':
        make(sys.argv[2], sys.argv[3] == '1')
    else:
        read(sys.argv[2])
