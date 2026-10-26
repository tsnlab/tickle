#!/usr/bin/env python3
"""eth_storm_stall_make.py - the experiment-only edit for eth_storm_stall_ab.sh (RESULTS `EWF`, 2026-10-27): a reader
that stops reading for a while just after it has asked for a whole sample.

  stall TREE MS   when the reader sends an ACKNACK naming at least 500 units (a whole sample missing: 723 units a
                  1 MiB sample, which only a whole-sample loss produces - the forced one of eth_storm_fix_make.py
                  drop, or a natural drain overrun), and no stall began in the last second, it takes in no large data
                  for MS ms: every FRAG_FIRST_L / FRAG_CONT_L / FRAG_RUN_L that reaches it meanwhile is dropped
                  unread, as by a socket that is full because nobody reads it. Its timers run on, so it goes on asking
                  for the sample. The writer's resend of the sample went in answer to that ACKNACK, so the stall falls
                  while that resend is pending and the reader shows no progress. Each stall logs "EWS stall <n>: ...",
                  with the large datagrams the previous one dropped (the treatment check).
Applied to every tree of the stall phase alike (M, A, S), so the arms differ only in their core. Asserts each anchor
occurs exactly once. Never production code.
"""
import sys


def edit(tree, pairs):
    p = tree + '/src/tickle.c'
    s = open(p).read()
    for old, new in pairs:
        assert s.count(old) == 1, old[:80]
        s = s.replace(old, new)
    open(p, 'w').write(s)


def stall(ms):
    return [
        ('''static void send_acknack_skipping(struct tt_Context* node, struct tt_WriterProxy* proxy, int low_bit, int high_bit,
                                  const struct acknack_skip* skips, int skip_count) {''',
         f'''static uint64_t ews_deaf_until; // EWS stall: no large data taken in before this (tt_get_ns())
static uint64_t ews_deaf_began;
static uint32_t ews_stalls;
static uint64_t ews_deaf_dropped;
static bool ews_deaf(void) {{
    if (ews_deaf_until != 0 && tt_get_ns() < ews_deaf_until) {{
        ews_deaf_dropped++;
        return true;
    }}
    return false;
}}
static void ews_maybe_stall(uint32_t named) {{
    uint64_t now = tt_get_ns();
    if (named >= 500U && (ews_deaf_began == 0 || now - ews_deaf_began >= tt_SECOND)) {{
        ews_stalls++;
        TT_LOG_WARNING("EWS stall %u: deaf %u ms after an ACKNACK naming %u units (the last stall dropped %llu)",
                       ews_stalls, {ms}U, named, (unsigned long long)ews_deaf_dropped);
        ews_deaf_dropped = 0;
        ews_deaf_began = now;
        ews_deaf_until = now + ((uint64_t){ms}U * tt_MILLISECOND);
    }}
}}
static void send_acknack_skipping(struct tt_Context* node, struct tt_WriterProxy* proxy, int low_bit, int high_bit,
                                  const struct acknack_skip* skips, int skip_count) {{'''),
        ('''    bool unicast = old_tx_tail == sizeof(struct tt_Header);
    if (!end_encode(node, submessage_header, true, unicast ? &target : NULL, unicast ? 1 : 0)) {
        rollback(node, old_tx_tail);
        return;
    }
    // Only a request that names the watermark itself can time its recovery''',
         '''    bool unicast = old_tx_tail == sizeof(struct tt_Header);
    uint32_t ews_named = 0; // EWS stall: counted before end_encode() moves the buffer
    for (uint16_t ews_word = 0; ews_word < wire_words; ews_word++) {
        ews_named += (uint32_t)__builtin_popcountll(acknack_header->bitmap[ews_word]);
    }
    ews_maybe_stall(ews_named);
    if (!end_encode(node, submessage_header, true, unicast ? &target : NULL, unicast ? 1 : 0)) {
        rollback(node, old_tx_tail);
        return;
    }
    // Only a request that names the watermark itself can time its recovery'''),
        ('''    case tt_SUBMESSAGE_TYPE_FRAG_FIRST_L:
    case tt_SUBMESSAGE_TYPE_FRAG_CONT_L:
        if (!self_sent) {''',
         '''    case tt_SUBMESSAGE_TYPE_FRAG_FIRST_L:
    case tt_SUBMESSAGE_TYPE_FRAG_CONT_L:
        if (!self_sent && !ews_deaf()) {'''),
        ('''    if (type == tt_SUBMESSAGE_TYPE_FRAG_RUN_L) {
        process_frag_run_large(node, header, buffer, head, body_tail, sender_ip, sender_port);''',
         '''    if (type == tt_SUBMESSAGE_TYPE_FRAG_RUN_L) {
        if (ews_deaf()) {
            return;
        }
        process_frag_run_large(node, header, buffer, head, body_tail, sender_ip, sender_port);'''),
    ]


def main():
    what, tree = sys.argv[1], sys.argv[2]
    if what == 'stall':
        edit(tree, stall(int(sys.argv[3])))
    else:
        sys.exit(f'unknown edit {what}')


if __name__ == '__main__':
    main()
