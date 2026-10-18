/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Differential test of the reliable receive bitmap (tt_WriterProxy.received_bitmap) against a naive
// model, and - through tests/ack_bitmap_differential.sh - against the tickle.c before
// tt_WriterProxy.received_words existed.
//
// received_words lets update_reliable_ack()'s watermark shift and highest-bit scan cost the words in use
// instead of the window (tickle.c, proxy_mark_received()). That is only an optimisation if every
// observable is unchanged, so this drives update_reliable_ack(), acknack_retry(),
// advance_past_unavailable() and the reorder buffer's un-receive with in-order runs, losses and
// repairs, reorder, duplicates, the window's edges, jumps past it, multi-seq spans and a seq_no
// wrap, at three window widths and under KEEP_LAST and KEEP_ALL, and after every step checks:
//   - the whole bitmap and the watermark against a naive bool-per-seq model of the semantics (bit j:
//     received(ack_seq_no + j)), written here without any of tickle.c's bitmap helpers;
//   - the highest received bit tickle.c reports against the model's;
//   - received_words itself: exactly the words up to the highest set bit, so every word above is zero.
// Run with --trace it prints, per step, the watermark, retry state, a hash of the whole bitmap and
// of every datagram sent (the ACKNACKs). ack_bitmap_differential.sh builds this file against the old
// and the new tickle.c with -DACK_DIFF_TRACE_ONLY (the old one has no received_words) and diffs the
// two traces, which is what shows that the ACKNACKs did not change.
//
// Positive controls: each scenario must have exercised what it is named for (ACKNACKs sent,
// retries into the back-off and no give-up, jumps, duplicates, multi-word bitmaps), or the check
// above decides nothing about it.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2
#define ENDPOINT_ID 0xaabbccdd
#define TEST_SENDER_IP 0x0a000001
#define TEST_SENDER_PORT 12345
#define MAX_BITS tt_RELIABLE_BITMAP_MAX_BITS

static bool trace = false;

// --- the naive model: one bool per seq_no from the watermark up ---

// a after b, serially (RFC 1982): seq_no wrap past 2^32 (tickle.c's seq_delta(), written out here on purpose).
static int32_t serial_diff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

struct model {
    bool started;
    uint32_t ack;
    uint32_t bits;
    bool recv[MAX_BITS];
};

static void model_shift(struct model* m, uint64_t n) {
    if (n >= m->bits) {
        memset(m->recv, 0, sizeof(m->recv));
        return;
    }
    memmove(m->recv, m->recv + n, (m->bits - n) * sizeof(bool));
    memset(m->recv + (m->bits - n), 0, n * sizeof(bool));
}

static void model_absorb(struct model* m) {
    while (m->recv[0]) {
        model_shift(m, 1);
        m->ack++;
    }
}

static void model_advance(struct model* m) {
    m->ack++;
    model_shift(m, 1);
    model_absorb(m);
}

static void model_data(struct model* m, uint32_t seq, uint16_t span) {
    if (!m->started) {
        m->started = true;
        m->ack = seq;
    }
    if (serial_diff(seq, m->ack) < 0) {
        return;
    }
    uint32_t offset = seq - m->ack;
    if (offset >= m->bits) {
        m->ack = seq;
        memset(m->recv, 0, sizeof(m->recv));
        model_advance(m);
        return;
    }
    // A record spanning several seq_nos (tt_Context.rx_seq_span) marks the rest of its span received too.
    m->recv[offset] = offset != 0; // bit 0 is the watermark itself, never held
    for (uint32_t i = 1; i < span; i++) {
        if (offset + i < m->bits) {
            m->recv[offset + i] = true;
        }
    }
    if (offset == 0) {
        model_advance(m);
    }
}

static int model_highest(const struct model* m) {
    for (int j = (int)m->bits - 1; j >= 0; j--) {
        if (m->recv[j]) {
            return j;
        }
    }
    return -1;
}

// --- the system under test ---

struct rig {
    struct tt_Context node;
    struct tt_TypeSupport topic;
    struct tt_Subscriber sub;
    uint64_t tracking[tt_MAX_PEER_COUNT * tt_RELIABLE_BITMAP_MAX_WORDS];
};

static struct rig rig;

static uint64_t fnv(uint64_t hash, const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
    }
    return hash;
}

static uint32_t step_sends;
static uint64_t step_send_hash;
static uint32_t total_sends;

static void capture(const void* buf, size_t len) {
    step_sends++;
    total_sends++;
    step_send_hash = fnv(step_send_hash, &len, sizeof(len));
    step_send_hash = fnv(step_send_hash, buf, len);
}

static void rig_init(uint16_t words) {
    memset(&rig, 0, sizeof(rig));
    node_init_locks(&rig.node);
    rig.node.id = LOCAL_NODE_ID;
    rig.node.tx_tail = sizeof(struct tt_Header);
    rig.node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    rig.topic.name = "test_topic";
    rig.topic.data_size = sizeof(uint32_t);
    struct tt_Subscriber* sub = &rig.sub;
    sub->endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub->endpoint.id = ENDPOINT_ID;
    sub->node = &rig.node;
    sub->topic = &rig.topic;
    sub->reliable = true;
    if (words != tt_RELIABLE_BITMAP_WORDS) {
        sub->tracking_bitmaps = rig.tracking;
        sub->tracking_words = words;
    }
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub->writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    rig.node.endpoint_count = 1;
    rig.node.endpoints[0] = (struct tt_Endpoint*)sub;
}

static struct tt_WriterProxy* rig_proxy(void) {
    return find_writer_proxy(&rig.sub, REMOTE_NODE_ID, 0);
}

// --- scenarios ---

struct scenario {
    const char* name;
    uint32_t start;
    int steps;
    // per-step weights, out of the sum
    int w_inorder, w_lose, w_repair, w_dup, w_window, w_jump, w_retry, w_heartbeat, w_unreceive;
    uint16_t span_max;
    bool in_order; // nothing lost: no ACKNACK may go out, and (new build) no bit is ever set
    bool expect_backoff, expect_jump, expect_dup, expect_multiword;
};

enum op_kind { OP_IN_ORDER, OP_LOSE, OP_REPAIR, OP_DUP, OP_WINDOW, OP_JUMP, OP_RETRY, OP_HEARTBEAT, OP_UNRECEIVE };

// Which op a draw of 0 .. sum-of-weights - 1 lands on.
static enum op_kind pick_op(const struct scenario* sc, uint32_t pick) {
    const int weights[] = {sc->w_inorder, sc->w_lose,  sc->w_repair,    sc->w_dup,      sc->w_window,
                           sc->w_jump,    sc->w_retry, sc->w_heartbeat, sc->w_unreceive};
    for (int kind = OP_IN_ORDER; kind < OP_UNRECEIVE; kind++) {
        if (pick < (uint32_t)weights[kind]) {
            return (enum op_kind)kind;
        }
        pick -= (uint32_t)weights[kind];
    }
    return OP_UNRECEIVE;
}

static uint64_t rng_state;

static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return n == 0 ? 0 : (uint32_t)(rng_state % n);
}

// One entry per seq_no the window can hold: lose_up_to() records a whole window's holes, and under KEEP_ALL a hole
// it could not record is never repaired (no give-up advances past it), so the watermark would stall for good. A fixed
// 4,096 did exactly that at -Dtt_LARGE_SAMPLES=1's 8,192-bit window: wrap/w128/all never wrapped.
#define LOST_MAX MAX_BITS
static uint32_t lost[LOST_MAX];
static int lost_count;

static void note_lost(uint32_t seq) {
    lost[lost_count < LOST_MAX ? lost_count++ : (int)rnd(LOST_MAX)] = seq;
}

static struct model model;

struct counts {
    // straddles: DATA from past 2^32 while the watermark had not wrapped yet - the case seq_delta() exists for
    // giveups: a retry that moved the watermark, which none may since 2026-10-15; backoffs: retries past
    // tt_RELIABLE_RETRY
    uint32_t giveups, backoffs, jumps, dups, max_highest, max_words, wraps, straddles, deviations;
};

static void check_step(const struct scenario* sc, int step, const char* op, uint32_t arg, int ret, struct counts* c) {
    struct tt_WriterProxy* proxy = rig_proxy();
    uint16_t words = proxy != NULL ? proxy_words(proxy) : 0;
    uint64_t bitmap_hash = 0xcbf29ce484222325ULL;
    if (proxy != NULL) {
        bitmap_hash = fnv(bitmap_hash, proxy->received_bitmap, (size_t)words * sizeof(uint64_t));
    }
    if (trace) {
        printf("%s %d %s %u ret=%d ack=%u retry=%u sched=%d aband=%u evict=%u bm=%016llx sends=%u sh=%016llx\n",
               sc->name, step, op, arg, ret, proxy != NULL ? proxy->ack_seq_no : 0U,
               proxy != NULL ? (unsigned)proxy->retry : 0U, proxy != NULL ? (int)proxy->acknack_scheduled : -1,
               rig.sub.gap_abandoned, rig.sub.gap_evicted, (unsigned long long)bitmap_hash, step_sends,
               (unsigned long long)step_send_hash);
    }
    if (proxy == NULL) {
        return;
    }
    bool ok = proxy->ack_seq_no == model.ack;
    int highest = -1;
    for (uint32_t j = 0; j < model.bits; j++) {
        bool bit = (proxy->received_bitmap[j / 64] >> (j % 64)) & 1U;
        ok = ok && bit == model.recv[j];
        if (bit) {
            highest = (int)j;
        }
    }
    ok = ok && highest == model_highest(&model);
#ifndef ACK_DIFF_TRACE_ONLY
    ok = ok && proxy_highest_received(proxy) == highest;
    ok = ok && proxy->received_words == (highest < 0 ? 0U : ((uint32_t)highest / 64U) + 1U); // exact, so a bound
    if (proxy->received_words > c->max_words) {
        c->max_words = proxy->received_words;
    }
#endif
    if (highest > (int)c->max_highest) {
        c->max_highest = (uint32_t)highest;
    }
    if (!ok && c->deviations++ < 3) {
        fprintf(stderr, "%s step %d (%s %u): tickle ack=%u highest=%d, model ack=%u highest=%d\n", sc->name, step, op,
                arg, proxy->ack_seq_no, highest, model.ack, model_highest(&model));
        test_failures++;
    }
}

// One scenario's run: what it has sent so far and what it has seen.
struct run {
    const struct scenario* sc;
    enum tt_WriterKeepAll keep_all;
    uint32_t next; // the next seq_no never sent
    struct counts c;
    bool straddled; // the wrap scenario's seq_no 1 has gone (choose())
};

// What one step did: a DATA to deliver (is_data), or something done already, named by op and arg.
struct step {
    const char* op;
    uint32_t arg;
    uint32_t seq;
    uint16_t span;
    bool is_data;
};

static void lose_up_to(struct run* r, uint32_t seq) {
    if (seq - r->next < (1U << 20)) {
        for (uint32_t s = r->next; s != seq && lost_count < LOST_MAX; s++) {
            note_lost(s);
        }
        r->next = seq + 1;
    }
}

static uint32_t window_edge_seq(void) {
    static const int64_t edges[] = {-1, 0, 1, 2, 63, 64, 65, 127, 128};
    uint32_t choice = rnd((sizeof(edges) / sizeof(edges[0])) + 4);
    if (choice < 4) {
        return model.ack + (model.bits - 2) + choice; // window-2 .. window+1
    }
    int64_t edge = edges[choice - 4];
    return (uint32_t)((int64_t)model.ack + (edge < 0 ? (int64_t)rnd(model.bits) : edge));
}

static void fire_retry(struct run* r, struct tt_WriterProxy* proxy, struct step* s) {
    s->op = "retry";
    s->is_data = false;
    if (proxy == NULL || !proxy->acknack_scheduled) {
        return;
    }
    (void)tt_Context_unschedule(&rig.node, acknack_retry, proxy); // as the timer firing pops it
    uint32_t abandoned = rig.sub.gap_abandoned;
    acknack_retry(&rig.node, tt_get_ns(), proxy);
    if (rig.sub.gap_abandoned != abandoned) {
        r->c.giveups++;
        model_advance(&model);
    }
    r->c.backoffs += proxy->retry > tt_RELIABLE_RETRY ? 1U : 0U;
    s->arg = 1;
}

static void heartbeat(struct run* r, struct tt_WriterProxy* proxy, struct step* s) {
    s->op = "heartbeat";
    s->is_data = false;
    if (proxy == NULL) {
        return;
    }
    uint32_t first = model.ack - 4 + rnd(model.bits + 16);
    s->arg = first;
    proxy->heartbeat_last_seq_no = r->next - 1;
    advance_past_unavailable(proxy, first);
    if (serial_diff(first, model.ack) > 0) {
        model_shift(&model, first - model.ack);
        model.ack = first;
        model_absorb(&model);
    }
    maybe_arm_acknack_retry(&rig.node, proxy);
    if (serial_diff(first, r->next) > 0) {
        r->next = first;
    }
}

// The reorder buffer's no-room path (tickle.c, after its overflow warning) clears a bit it had set.
static void unreceive(struct tt_WriterProxy* proxy, struct step* s) {
    s->op = "unreceive";
    s->is_data = false;
    int candidates = 0;
    for (uint32_t j = 0; j < model.bits; j++) {
        candidates += model.recv[j] ? 1 : 0;
    }
    if (proxy == NULL || candidates == 0) {
        return;
    }
    int target = (int)rnd((uint32_t)candidates);
    for (uint32_t j = 0; j < model.bits; j++) {
        if (model.recv[j] && target-- == 0) {
            bitmap_clear_bit(proxy->received_bitmap, j);
#ifndef ACK_DIFF_TRACE_ONLY
            proxy_trim_received(proxy); // as that path does
#endif
            model.recv[j] = false;
            note_lost(model.ack + j);
            s->arg = j;
            return;
        }
    }
}

static struct step choose(struct run* r, enum op_kind kind, struct tt_WriterProxy* proxy) {
    struct step s = {"data", 0, 0, 1, true};
    const struct scenario* sc = r->sc;
    // The wrap scenario's first in-order step sends seq_no 1 - past 2^32, within the window, while the watermark has
    // not wrapped yet - so the case its control asks for happens on every run rather than when the draw allows it
    // (wrap/w16/all once crossed by a Heartbeat and a jump, with no DATA ever meeting an unwrapped watermark).
    if (sc->start >= (1U << 31) && kind == OP_IN_ORDER && !r->straddled && model.started &&
        serial_diff(1U, model.ack) > 0 && serial_diff(1U, model.ack) < (int32_t)(model.bits / 2U)) {
        r->straddled = true;
        lose_up_to(r, 1U);
        s.seq = 1U;
        return s;
    }
    switch (kind) {
    case OP_IN_ORDER:
        s.span = (uint16_t)(1 + rnd(sc->span_max));
        s.seq = r->next;
        r->next += s.span;
        break;
    case OP_LOSE:
        s.op = "lose";
        note_lost(r->next);
        s.arg = r->next++;
        s.is_data = false;
        break;
    case OP_REPAIR:
        s.op = "repair";
        s.seq = lost_count > 0 ? lost[rnd((uint32_t)lost_count)] : r->next - 1;
        // Under KEEP_ALL half the repairs answer the watermark's own hole, the first seq_no an ACKNACK asks for, which
        // such a writer still holds. A pick from the lost list alone lands on it once in a window's worth of holes, and
        // with no give-up to move past it, at 4,096 and 8,192 bits the stream stalled below the wrap it was named for
        // (wrap/w64/all on most seeds, wrap/w128/all on the fixed one). Not under KEEP_LAST: its writer may no longer
        // hold the sample, and repairing the watermark there starved the give-ups that path's control asked for until
        // 2026-10-15, and the back-offs it asks for since.
        if (r->keep_all == tt_WRITER_KEEP_ALL_YES && lost_count > 0 && model.ack != r->next && rnd(2) == 0) {
            s.seq = model.ack;
        }
        break;
    case OP_DUP: {
        s.op = "dup";
        uint32_t sent = r->next - sc->start;
        s.seq = r->next - 1 - rnd(model.bits + 8 < sent ? model.bits + 8 : sent);
        break;
    }
    case OP_WINDOW:
        s.op = "window";
        s.seq = window_edge_seq();
        lose_up_to(r, s.seq);
        break;
    case OP_JUMP:
        s.op = "jump";
        s.seq = model.ack + model.bits + rnd(3 * model.bits);
        if (serial_diff(s.seq, r->next) >= 0) {
            r->next = s.seq + 1;
        }
        lost_count = 0; // a jump abandons the window's holes - across 2^32 too, now that seq_no compare serially

        break;
    case OP_RETRY:
        fire_retry(r, proxy, &s);
        break;
    case OP_HEARTBEAT:
        heartbeat(r, proxy, &s);
        break;
    case OP_UNRECEIVE:
        unreceive(proxy, &s);
        break;
    }
    return s;
}

static int deliver(struct run* r, struct tt_WriterProxy* proxy, const struct step* s) {
    if (proxy != NULL && serial_diff(s->seq, model.ack) >= 0) {
        uint32_t offset = s->seq - model.ack;
        r->c.dups += offset != 0 && offset < model.bits && model.recv[offset] ? 1U : 0U;
        r->c.jumps += offset >= model.bits ? 1U : 0U;
        r->c.straddles += s->seq < model.ack ? 1U : 0U; // ahead serially, behind unsigned
    }
    rig.node.rx_seq_span = s->span;
    model_data(&model, s->seq, s->span);
    bool ret = update_reliable_ack(&rig.node, &rig.sub, s->seq, REMOTE_NODE_ID, 0, TEST_SENDER_IP, TEST_SENDER_PORT);
    if (proxy == NULL && rig_proxy() != NULL) {
        rig_proxy()->keep_all = r->keep_all; // the writer's announce, seen with its first DATA
    }
    return ret ? 1 : 0;
}

static void check_controls(const struct run* r, const char* name, uint16_t words, uint32_t sends) {
    const struct scenario* sc = r->sc;
    const struct counts* c = &r->c;
    bool quiet_ok = sc->in_order ? sends == 0 && c->max_words == 0 : sends > 0;
    bool wrap_ok = sc->start < (1U << 31) || (c->wraps > 0 && c->straddles > 0);
    // No policy gives a gap up on a retry count (acknack_retry(), 2026-10-15): a retry never moves the watermark, and
    // under KEEP_LAST the scenarios named for it retried a gap into the back-off, where they used to give it up.
    bool giveup_ok =
        c->giveups == 0 && (!sc->expect_backoff || r->keep_all != tt_WRITER_KEEP_ALL_NO || c->backoffs > 0);
    bool seen_ok = (!sc->expect_jump || c->jumps > 0) && (!sc->expect_dup || c->dups > 0);
    bool wide_ok = !sc->expect_multiword || words == tt_RELIABLE_BITMAP_WORDS || c->max_highest >= 128;
    if (!(quiet_ok && wrap_ok && giveup_ok && seen_ok && wide_ok)) {
        fprintf(stderr,
                "%s: positive control not met (sends %u giveups %u backoffs %u jumps %u dups %u highest %u words %u "
                "wraps %u straddles %u)\n",
                name, sends, c->giveups, c->backoffs, c->jumps, c->dups, c->max_highest, c->max_words, c->wraps,
                c->straddles);
        test_failures++;
    }
    if (!trace) {
        printf("  %-24s steps %d sends %u giveups %u backoffs %u jumps %u dups %u highest-bit %u words %u wraps %u "
               "straddles %u deviations %u\n",
               name, sc->steps, sends, c->giveups, c->backoffs, c->jumps, c->dups, c->max_highest, c->max_words,
               c->wraps, c->straddles, c->deviations);
    }
}

static void run_scenario(const struct scenario* sc, uint16_t words, enum tt_WriterKeepAll keep_all, uint64_t seed) {
    test_mock_reset();
    test_mock_send_hook = capture;
    rig_init(words);
    memset(&model, 0, sizeof(model));
    model.bits = (uint32_t)words * 64U;
    rng_state = seed;
    lost_count = 0;
    struct run r = {sc, keep_all, sc->start, {0}};
    uint32_t sends_before = total_sends;
    int total_weight = sc->w_inorder + sc->w_lose + sc->w_repair + sc->w_dup + sc->w_window + sc->w_jump + sc->w_retry +
                       sc->w_heartbeat + sc->w_unreceive;
    char name[96];
    snprintf(name, sizeof(name), "%s/w%u/%s", sc->name, (unsigned)words,
             keep_all == tt_WRITER_KEEP_ALL_YES ? "all" : "last");
    struct scenario named = *sc;
    named.name = name;

    for (int step = 0; step < sc->steps; step++) {
        uint32_t ack_before = model.ack;
        step_sends = 0;
        step_send_hash = 0xcbf29ce484222325ULL;
        test_mock_now += 1000;
        struct tt_WriterProxy* proxy = rig_proxy();
        enum op_kind kind = pick_op(sc, rnd((uint32_t)total_weight));
        struct step s = choose(&r, step == 0 ? OP_IN_ORDER : kind, proxy);
        int ret = 0;
        if (s.is_data) {
            s.arg = s.seq;
            ret = deliver(&r, proxy, &s);
        }
        check_step(&named, step, s.op, s.arg, ret, &r.c);
        r.c.wraps += model.ack < ack_before && ack_before - model.ack > (1U << 31) ? 1U : 0U;
    }

    check_controls(&r, name, words, total_sends - sends_before);
    // Leave no timer behind for the next scenario's fresh rig.
    struct tt_WriterProxy* proxy = rig_proxy();
    if (proxy != NULL && proxy->acknack_scheduled) {
        (void)tt_Context_unschedule(&rig.node, acknack_retry, proxy);
    }
}

static const struct scenario scenarios[] = {
    // name           start         steps inord lose rep dup win jump retry hb unrecv span in_order backoff jump dup
    // multiword
    {"in_order", 1, 3000, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, true, false, false, false, false},
    {"in_order_span", 1, 2000, 1, 0, 0, 0, 0, 0, 0, 0, 0, 6, true, false, false, false, false},
    {"loss_repair", 1, 4000, 80, 6, 6, 0, 0, 0, 8, 0, 0, 1, false, true, false, false, false},
    {"reorder_dup", 1, 4000, 50, 10, 25, 10, 0, 0, 5, 0, 0, 1, false, true, false, true, false},
    {"window_edges", 1, 3000, 30, 2, 20, 4, 25, 0, 6, 0, 0, 1, false, true, false, true, true},
    {"jumps", 1, 2000, 40, 5, 10, 5, 10, 5, 5, 0, 0, 3, false, false, true, true, false},
    {"heartbeat", 1, 3000, 40, 8, 10, 4, 10, 0, 6, 6, 3, 2, false, false, false, true, true},
    // Starts 64 short of 2^32. Its control asks for the wrap and for DATA past it while the watermark lagged behind -
    // which update_reliable_ack() took as late until it compared serially (seq_delta()), so a KEEP_LAST stream at
    // 4,096 or 8,192 bits crossed 2^32 only one give-up at a time and needed 12,000 steps to cross at all; kept.
    {"wrap", 0xffffffc0U, 12000, 40, 6, 10, 5, 8, 2, 6, 2, 2, 2, false, false, false, false, false},
    {"random", 1, 6000, 20, 10, 15, 10, 15, 2, 10, 5, 5, 4, false, true, true, true, true},
};

int main(int argc, char** argv) {
    trace = argc > 1 && strcmp(argv[1], "--trace") == 0;
    tt_current_log_level = TT_LOG_NONE;
    static const uint16_t widths[] = {tt_RELIABLE_BITMAP_WORDS, 16, tt_RELIABLE_BITMAP_MAX_WORDS};
    static const enum tt_WriterKeepAll policies[] = {tt_WRITER_KEEP_ALL_NO, tt_WRITER_KEEP_ALL_YES};
    uint64_t seed = 0x9e3779b97f4a7c15ULL;
    for (size_t s = 0; s < sizeof(scenarios) / sizeof(scenarios[0]); s++) {
        for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
            for (size_t p = 0; p < sizeof(policies) / sizeof(policies[0]); p++) {
                seed += 0x632be59bd9b4e019ULL;
                run_scenario(&scenarios[s], widths[w], policies[p], seed);
            }
        }
    }
    if (!trace) {
        printf("test_ack_bitmap_diff: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    }
    return test_result();
}
