#!/usr/bin/env python3
"""eth_storm_fix_make.py - experiment-only edits for eth_storm_fix_ab.sh (RESULTS `EWS`): the forced trigger and the
pinned timeout of eth_storm_force.py, written against production tickle.c (no trace patch), so they apply to main and
to both candidates alike.

  pin TREE        the large repair timeout pinned at 1465 x 1.024 us (~1.5 ms, the storm rep's value) wherever srtt
                  is measured - the provocation that made main storm in 10 of 20 forced reps (`EWS`, force1 FP).
  drop TREE N     the first send of every N-th large sample (from the 60th) goes nowhere - large_send_batch() reports
                  it sent - so the reader names the whole sample, as after its receive drain overran. Each such sample
                  logs "EWS forced loss <n>" (the treatment check).
  backoff TREE    candidate F: a unit resent k times waits timeout << min(k - 1, 6) before the timeout releases it
                  again (the proposal of `EWS`, RFC 6298 5.5 per unit). Never production code on this branch.
Every edit asserts its anchor occurs exactly once. Never production code.
"""
import sys


def edit(tree, pairs):
    p = tree + '/src/tickle.c'
    s = open(p).read()
    for old, new in pairs:
        assert s.count(old) == 1, old[:80]
        s = s.replace(old, new)
    open(p, 'w').write(s)


PIN = [('''        order->timeout = (uint32_t)(((uint64_t)ack->repair_srtt_ns * 2U) >> LARGE_REPAIR_SHIFT) + 1U;''',
        '''        order->timeout = 1465U; // EWS: the timeout pinned at ~1.5 ms, the storm rep's value''')]

BACKOFF = [('''    if (now - since >= (order->timeout != 0 ? order->timeout : slot->hold)) {''', None),
           ('''    if (now - slot->resent_at >= (order->timeout != 0 ? order->timeout : slot->hold)) {''',
            '''    uint32_t ews_base = order->timeout != 0 ? order->timeout : slot->hold;
    uint32_t ews_shift = slot->resends > 1U ? (uint32_t)(slot->resends - 1U) : 0U;
    ews_shift = ews_shift < 6U ? ews_shift : 6U;
    if (now - slot->resent_at >= (ews_shift == 0U || ews_base <= (UINT32_MAX >> ews_shift) ? ews_base << ews_shift
                                                                                          : UINT32_MAX)) {
        // EWS candidate F: per-unit back-off, x2 a resend past the first, at most x64''')]


def drop(n):
    return [
        ('''static int32_t large_send_batch(struct tt_Context* node, const struct tt_OutDatagram* batch, uint32_t count,
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
#if tt_SEGMENT_ENABLED'''),
        ('''static bool large_send_some(struct tt_Context* node, struct tt_Publisher* pub, struct tt_LargeRecord* record,
                            bool* progress, bool* failed) {
    struct tt_LargeCursor* cursor = &pub->large_cursor;''',
         f'''static bool large_send_some_ews(struct tt_Context* node, struct tt_Publisher* pub, struct tt_LargeRecord* record,
                                bool* progress, bool* failed);
static bool large_send_some(struct tt_Context* node, struct tt_Publisher* pub, struct tt_LargeRecord* record,
                            bool* progress, bool* failed) {{
    const struct tt_LargeCursor* ews_cursor = &pub->large_cursor;
    if (ews_cursor->next_dest == 0 && ews_cursor->next_index == 0 && record->seq_no != ews_drop_seq) {{
        ews_samples++;
        if (ews_samples >= 60U && ews_samples % {n}U == 0U) {{
            ews_drop_seq = record->seq_no;
            ews_dropped++;
            TT_LOG_WARNING("EWS forced loss %u: sample at seq_no %u", ews_dropped, record->seq_no);
        }}
    }}
    ews_drop_now = record->seq_no == ews_drop_seq;
    bool ews_done = large_send_some_ews(node, pub, record, progress, failed);
    ews_drop_now = 0;
    return ews_done;
}}
static bool large_send_some_ews(struct tt_Context* node, struct tt_Publisher* pub, struct tt_LargeRecord* record,
                                bool* progress, bool* failed) {{
    struct tt_LargeCursor* cursor = &pub->large_cursor;'''),
    ]


def main():
    what, tree = sys.argv[1], sys.argv[2]
    if what == 'pin':
        edit(tree, PIN)
    elif what == 'drop':
        edit(tree, drop(int(sys.argv[3])))
    elif what == 'backoff':
        p = tree + '/src/tickle.c'
        s = open(p).read()
        # F is written against main's large_resend_due(); a tree that already holds G's line is refused
        assert s.count(BACKOFF[0][0]) == 0, 'backoff: this tree holds candidate G'
        edit(tree, BACKOFF[1:])
    else:
        sys.exit(f'unknown edit {what}')


if __name__ == '__main__':
    main()
