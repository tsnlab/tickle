#!/usr/bin/env python3
"""eth_storm_force.py - the EWS forced-trigger A/B (RESULTS `EWS`): make and read experiment-only trees.

WHY. The traced storm (provoke/P1 rep 5, timeout pinned at ~1.5 ms) started at the one whole sample this rep lost
(the reader's receive drain overran: 722 units named at once). The writer resent the 722 in one batch, which the
loaded link paced to 9.5 ms; it read no ACKNACK meanwhile, then judged the five that had queued - sent before any
repair could arrive - by the time it read them: RACK released all 722 again, every unit had gone twice (no Karn
sample can time it after that), and each later ACKNACK released by the timeout every unit it named. A calm rep with
the same burst at srtt 756 us held every request instead: its first repairs were shown arrived within ~5 ms, and the
Karn samples took srtt (and the timeout) above the 4.9 ms the repairs took. Whole-sample losses are rare (~6% of
reps); this run makes one every FORCE_EVERY-th sample (default 90: every 3 s at 30 Hz, from sample 60).

  make TREE PIN BACKOFF DROP   edit TREE/src/tickle.c (a copy of this checkout's, trace hooks included). PIN=1: the
                      large repair timeout pinned at 1465 x 1.024 us (~1.5 ms) wherever srtt is measured. BACKOFF=1: a
                      unit already resent waits base << min(resends - 1, 6) before the timeout releases it. DROP=N: the
                      first send of every N-th large sample (from the 60th) goes nowhere - large_send_batch() reports
                      it sent - so the reader names the whole sample, as after a drain overrun. Never production code.
  read OUT            the reading, fixed before the run (arms FM = main, FB = main + back-off, FP = pinned,
                      FPB = pinned + back-off; all with DROP):
      STORM: the writer's retransmitted >= 100,000. TREATMENT: an arm whose reps' ping.log carry no
         "EWS forced loss" line is VOID (the drop never happened).
      LOOP: FP storms in >= 3 of its reps -> REACHABLE (a ~1.5 ms timeout + a whole-sample burst starts it);
         0 -> NOT REACHED; else WEAK.
      BACK-OFF (read only when LOOP is REACHABLE): FPB 0 storms and the one-sided Fisher p of FPB against FP <= 0.05
         -> BREAKS IT; else NOT SHOWN.
      MAIN (descriptive): FM and FB storms, resends median, timeout releases median.
"""
import glob
import math
import re
import statistics
import sys


def make(tree, pin, backoff, drop):
    p = tree + '/src/tickle.c'
    s = open(p).read()

    def rep(old, new):
        nonlocal s
        assert s.count(old) == 1, old[:70]
        s = s.replace(old, new)

    if pin:
        rep('''        order->timeout = (uint32_t)(((uint64_t)ack->repair_srtt_ns * 2U) >> LARGE_REPAIR_SHIFT) + 1U;''',
            '''        order->timeout = 1465U; // EWS: the timeout pinned at ~1.5 ms, the storm rep's value''')
    if backoff:
        rep('''    if (now - slot->resent_at >= (order->timeout != 0 ? order->timeout : slot->hold)) {''',
            '''    uint32_t ews_base = order->timeout != 0 ? order->timeout : slot->hold;
    uint32_t ews_shift = slot->resends > 1U ? (uint32_t)(slot->resends - 1U) : 0U;
    ews_shift = ews_shift < 6U ? ews_shift : 6U;
    if (now - slot->resent_at >= (ews_base << ews_shift)) { // EWS: per-unit back-off, x2 a resend''')
    if drop:
        rep('''static int32_t large_send_batch(struct tt_Context* node, const struct tt_OutDatagram* batch, uint32_t count,
                                const struct tt_Peer* dest) {
#if tt_SEGMENT_ENABLED''', '''static int ews_drop_now; // EWS: this first send goes nowhere (large_send_some())
static uint32_t ews_samples;
static uint32_t ews_drop_seq;
static uint32_t ews_dropped;
static int32_t large_send_batch(struct tt_Context* node, const struct tt_OutDatagram* batch, uint32_t count,
                                const struct tt_Peer* dest) {
    if (ews_drop_now) {
        return (int32_t)count;
    }
#if tt_SEGMENT_ENABLED''')
        rep('''    node->tx_seq_span = 1;
    R4T('C', record->seq_no + cursor->next_index, record->count);''', f'''    node->tx_seq_span = 1;
    R4T('C', record->seq_no + cursor->next_index, record->count);
    if (cursor->next_dest == 0 && cursor->next_index == 0 && record->seq_no != ews_drop_seq) {{
        ews_samples++;
        if (ews_samples >= 60U && ews_samples % {drop}U == 0U) {{
            ews_drop_seq = record->seq_no;
            ews_dropped++;
            TT_LOG_WARNING("EWS forced loss %u: sample at seq_no %u", ews_dropped, record->seq_no);
            R4T('Z', record->seq_no, ews_dropped);
        }}
    }}
    ews_drop_now = record->seq_no == ews_drop_seq;''')
        rep('''        R4T('E', record->seq_no + record->count, 1);
        cursor->next_dest++;''', '''        R4T('E', record->seq_no + record->count, 1);
        ews_drop_now = 0;
        cursor->next_dest++;''')
        rep('''        if (!done) {
            R4T('E', record->seq_no + cursor->next_index, 0);
            return false;''', '''        if (!done) {
            R4T('E', record->seq_no + cursor->next_index, 0);
            ews_drop_now = 0;
            return false;''')
    open(p, 'w').write(s)


def arm(out, label):
    rows, treated = [], 0
    for run in sorted(glob.glob(f'{out}/{label}/runs/*')):
        try:
            pl = open(run + '/ping.log', errors='replace').read()
        except OSError:
            continue
        m = re.search(r'^RESULT: role=pub.*?retransmitted=(\d+)', pl, re.M)
        t = re.search(r'released_timeout=(\d+)', pl)
        if m:
            rows.append((int(m.group(1)), int(t.group(1)) if t else -1, run.rsplit('-', 1)[1],
                         pl.count('EWS forced loss')))
            treated += 'EWS forced loss' in pl
    return rows, treated


def fisher_one_sided(a, n1, b, n2):
    # P(X <= a) for X ~ hypergeometric: a storms of n1 in the treated arm, b of n2 in the control
    k, n = a + b, n1 + n2
    tot = math.comb(n, k)
    return sum(math.comb(n1, i) * math.comb(n2, k - i) for i in range(0, a + 1) if k - i <= n2) / tot


def read(out):
    res = {}
    for label in ('FM', 'FB', 'FP', 'FPB'):
        rows, treated = arm(out, label)
        storms = sum(1 for r in rows if r[0] >= 100_000)
        med = statistics.median(r[0] for r in rows) if rows else None
        tmed = statistics.median(r[1] for r in rows) if rows else None
        res[label] = (rows, storms)
        void = rows and treated == 0
        print(f'{label}: {len(rows)} reps, treated {treated} (forced losses a rep: '
              f'{sorted(set(r[3] for r in rows))}){" VOID" if void else ""}, STORM {storms}, resends median {med}, '
              f'timeout releases median {tmed}')
        print(f'    per rep (resends, timeout releases): {[(r[0], r[1]) for r in rows]}')
    rows, storms = res['FP']
    loop = 'REACHABLE' if storms >= 3 else 'NOT REACHED' if storms == 0 else 'WEAK'
    print(f'LOOP {loop} (FP {storms} of {len(rows)})')
    if loop == 'REACHABLE':
        rb, sb = res['FPB']
        p = fisher_one_sided(sb, len(rb), storms, len(rows))
        print(f'BACK-OFF {"BREAKS IT" if sb == 0 and p <= 0.05 else "NOT SHOWN"} (FPB {sb} of {len(rb)} against FP '
              f'{storms} of {len(rows)}, one-sided p {p:.4f})')
    else:
        print('BACK-OFF not read (LOOP is not REACHABLE)')
    print(f'MAIN FM {res["FM"][1]} of {len(res["FM"][0])} storms, FB {res["FB"][1]} of {len(res["FB"][0])}')


if __name__ == '__main__':
    if sys.argv[1] == 'make':
        make(sys.argv[2], sys.argv[3] == '1', sys.argv[4] == '1', int(sys.argv[5]))
    else:
        read(sys.argv[2])
