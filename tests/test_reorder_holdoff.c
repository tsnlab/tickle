/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The reorder hold-off (ab/reorder-holdoff, stage S3; tickle.c "Reorder hold-off"): a RELIABLE reader holds a gap
// back from its ACKNACKs for a while, in case it is a reordering, and NACKs it only if it is still open then.
//
// The binding requirement first (the user's decision (i), 2026-11-03): on a stream that never reorders, every auto
// candidate (B, D, E) sends byte for byte the ACKNACKs off sends - and off sends what main (ad4f7fe8) sent. One script
// of losses, repairs, retries, HEARTBEATs and repair reveals, no late original anywhere, is run under every arm with
// every datagram the reader sends recorded; the logs must be equal. Its controls: a fixed hold on the same script must
// differ (the log sees a hold), and the same script with one late original must make auto differ (the gate can open).
// Off against main: the off log's digest must be the one this file printed when built against main's src/tickle.c (a
// tree without the hold-off compiles this file to the digest printer alone).
//
// Then each mechanism, each with its control: a held gap filled by its late original is never NACKed (off NACKs it);
// a held gap that stays open is NACKed when its hold has passed and not before; a gap opened while the retry runs is
// held from the retry; the window-half rule releases early; B releases on distance and D on distance after a quarter
// of its time; a HEARTBEAT's tail is held; auto's gate opens on a spurious NACK and closes after 64 clean recoveries;
// and the options resolve, fold and refuse as tickle.h says.

#define tt_RELIABLE_STATS 1

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

static int32_t stub_data_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_data_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, data, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static int32_t stub_data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len,
                                bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(data, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void stub_data_free(struct tt_Data* data) {
    (void)data;
}

static void stub_subscriber_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                     struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
}

#define TEST_REORDER_SLOTS 64
#define TEST_REORDER_SLOT_BYTES (sizeof(struct tt_ReorderSlot) + 64)
static uint64_t test_reorder_storage[TEST_REORDER_SLOTS * TEST_REORDER_SLOT_BYTES / sizeof(uint64_t)];

static struct tt_Context node;
static struct tt_TypeSupport topic;
static struct tt_Subscriber sub;
static struct tt_Header header;

// Every datagram the reader sends, in order: its length, then its bytes.
#define SEND_LOG_BYTES (1U << 20)
static uint8_t send_log[SEND_LOG_BYTES];
static size_t send_log_len;
static bool send_log_overflow;

static void record_send(const void* buf, size_t len) {
    if (send_log_len + sizeof(uint32_t) + len > SEND_LOG_BYTES) {
        send_log_overflow = true;
        return;
    }
    uint32_t length = (uint32_t)len;
    memcpy(send_log + send_log_len, &length, sizeof(length));
    memcpy(send_log + send_log_len + sizeof(length), buf, len);
    send_log_len += sizeof(length) + len;
}

// FNV-1a, 64 bits: the send log's digest.
#define FNV_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL
static uint64_t fnv1a(const uint8_t* bytes, size_t len) {
    uint64_t hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

static void reset(void) {
    test_mock_reset();
    tt_reliable_stats_reset();
    test_mock_now = 1000 * tt_MICROSECOND; // non-zero: a 0 timestamp means "unset"
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = LOCAL_NODE_ID;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    node.rx_seq_span = 1;

    memset(&topic, 0, sizeof(topic));
    topic.name = "test_topic";
    topic.data_size = sizeof(uint32_t);
    topic.data_encode_size = stub_data_encode_size;
    topic.data_encode = stub_data_encode;
    topic.data_decode = stub_data_decode;
    topic.data_free = stub_data_free;

    memset(&sub, 0, sizeof(sub));
    sub.endpoint.kind = tt_KIND_TOPIC_SUBSCRIBER;
    sub.endpoint.id = ENDPOINT_ID;
    sub.node = &node;
    sub.topic = &topic;
    sub.callback = stub_subscriber_callback;
    sub.reliable = true;
    sub.reorder_storage = test_reorder_storage;
    sub.reorder_slots = TEST_REORDER_SLOTS;
    sub.reorder_slot_bytes = TEST_REORDER_SLOT_BYTES;
    memset(test_reorder_storage, 0, sizeof(test_reorder_storage));
    for (int i = 0; i < tt_MAX_PEER_COUNT; i++) {
        sub.writers[i].context_id = tt_CONTEXT_ID_INVALID;
    }
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&sub;

    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = REMOTE_NODE_ID;

    send_log_len = 0;
    send_log_overflow = false;
    test_mock_send_hook = record_send;
}

// Runs every scheduled entry due by `until`, each at its own time, and leaves the clock at `until`.
static void run_until(uint64_t until) {
    for (;;) {
        struct tt_TCB* tcb = peek_scheduler(&node);
        if (tcb == NULL || tcb->time > until) {
            break;
        }
        struct tt_TCB due = *tcb;
        pop_scheduler(&node);
        if (due.time > test_mock_now) {
            test_mock_now = due.time;
        }
        due.function(&node, due.time, due.param);
    }
    if (until > test_mock_now) {
        test_mock_now = until;
    }
}

static void advance_us(uint32_t us) {
    run_until(test_mock_now + ((uint64_t)us * tt_MICROSECOND));
}

// One DATA of seq_no `after_us` after the last, timers due meanwhile run first; `repair`: addressed to this node.
static void arrive(uint32_t seq_no, uint32_t after_us, bool repair) {
    advance_us(after_us);
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)node.rx_buffer;
    data_header->endpoint_id = ENDPOINT_ID;
    data_header->seq_no = seq_no;
    data_header->timestamp = (uint64_t)seq_no * 100U;
    data_header->entity_id = 0;
    uint32_t tail = sizeof(struct tt_DataHeader);
    memcpy(node.rx_buffer + tail, &seq_no, sizeof(seq_no));
    node.rx_targeted = repair;
    EXPECT_TRUE(
        process_data(&node, &header, node.rx_buffer, 0, tail + sizeof(seq_no), TEST_SENDER_IP, TEST_SENDER_PORT));
    node.rx_targeted = false;
}

// A HEARTBEAT announcing first..last, asking for an answer unless `final`.
static void heartbeat(uint32_t first, uint32_t last, bool final, uint32_t after_us) {
    advance_us(after_us);
    struct tt_HeartbeatHeader* heartbeat_header = (struct tt_HeartbeatHeader*)node.rx_buffer;
    memset(heartbeat_header, 0, sizeof(*heartbeat_header));
    heartbeat_header->endpoint_id = ENDPOINT_ID;
    heartbeat_header->first_available_seq_no = first;
    heartbeat_header->last_seq_no = last;
    heartbeat_header->entity_id = 0;
    heartbeat_header->flags = final ? tt_HEARTBEAT_FLAG_FINAL : 0U;
    EXPECT_TRUE(process_heartbeat(&node, &header, node.rx_buffer, 0, sizeof(*heartbeat_header), TEST_SENDER_IP,
                                  TEST_SENDER_PORT));
}

static struct tt_WriterProxy* proxy_of(void) {
    return find_writer_proxy(&sub, REMOTE_NODE_ID, 0);
}

// The last datagram sent, as an ACKNACK; NULL if it was none.
static const struct tt_AckNackHeader* last_acknack(void) {
    size_t submessage_off = sizeof(struct tt_Header);
    size_t acknack_off = submessage_off + sizeof(struct tt_SubmessageHeader);
    if (test_mock_send_last_len < acknack_off + sizeof(struct tt_AckNackHeader)) {
        return NULL;
    }
    const struct tt_SubmessageHeader* submessage =
        (const struct tt_SubmessageHeader*)(test_mock_send_last_buf + submessage_off);
    if (submessage->type != tt_SUBMESSAGE_TYPE_ACKNACK) {
        return NULL;
    }
    return (const struct tt_AckNackHeader*)(test_mock_send_last_buf + acknack_off);
}

// Whether the last ACKNACK asked for seq_no.
static bool last_acknack_names(uint32_t seq_no) {
    const struct tt_AckNackHeader* acknack = last_acknack();
    if (acknack == NULL || seq_delta(seq_no, acknack->seq_no) < 0) {
        return false;
    }
    uint32_t bit = seq_no - acknack->seq_no;
    if (bit / 64U >= acknack->bitmap_words) {
        return false;
    }
    uint64_t word = 0;
    memcpy(&word, (const uint8_t*)acknack + sizeof(*acknack) + ((bit / 64U) * sizeof(uint64_t)), sizeof(word));
    return ((word >> (bit % 64U)) & 1U) != 0;
}

// ---- The no-reorder script and the arms ----

#define ARM_OFF 0
#define ARM_B 1
#define ARM_D 2
#define ARM_E 3
#define ARM_FIXED_1MS 4
#define ARM_COUNT 5

static void set_arm(int arm) {
#ifdef tt_REORDER_HOLDOFF_OFF
    switch (arm) {
    case ARM_B:
        node.holdoff_mode = tt_HOLDOFF_MODE_AUTO;
        node.holdoff_policy = tt_HOLDOFF_POLICY_B;
        break;
    case ARM_D:
        node.holdoff_mode = tt_HOLDOFF_MODE_AUTO;
        node.holdoff_policy = tt_HOLDOFF_POLICY_D;
        break;
    case ARM_E:
        node.holdoff_mode = tt_HOLDOFF_MODE_AUTO;
        node.holdoff_policy = tt_HOLDOFF_POLICY_E;
        break;
    case ARM_FIXED_1MS:
        node.holdoff_mode = tt_HOLDOFF_MODE_FIXED;
        node.holdoff_ns = (uint32_t)tt_MILLISECOND;
        break;
    default:
        node.holdoff_mode = tt_HOLDOFF_MODE_OFF;
        break;
    }
#else
    (void)arm;
#endif
}

// A stream with losses and no reorder: every gap is filled by a repair, never by its original. Gaps open with no retry
// armed (N1) and while it runs (N2); retries fire (N3); HEARTBEATs reveal a tail and ask for answers (N4); with
// `reveal` a repair above the watermark re-asks (N5). More than 64 gaps recovered cleanly. `late_at` >= 0: in that
// round the lost datagram's original arrives late instead of its repair - the one reorder, for the control.
static void run_script(uint8_t reveal, int late_at) {
    node.repair_reveal = reveal;
    uint32_t seq = 1;
    for (; seq <= 10; seq++) {
        arrive(seq, 50, false);
    }
    arrive(12, 50, false); // gap 11: no retry armed
    arrive(13, 50, false);
    arrive(16, 50, false); // gap 14-15 while the retry runs
    advance_us(1500);      // the retry
    arrive(11, 100, true);
    arrive(14, 100, true);
    advance_us(1200);
    arrive(15, 100, true);
    for (seq = 17; seq <= 20; seq++) {
        arrive(seq, 50, false);
    }
    heartbeat(1, 24, false, 50); // a tail 21-24, no retry armed
    heartbeat(1, 24, false, 300);
    for (seq = 21; seq <= 24; seq++) {
        arrive(seq, 50, true);
    }
    seq = 25;
    for (int round = 0; round < 90; round++) {
        arrive(seq, 100, false);
        if (round % 5 == 4) { // two gaps; the later one's repair first (a reveal, when on)
            arrive(seq + 2, 100, false);
            arrive(seq + 4, 100, false);
            advance_us(200);
            arrive(seq + 3, 50, true);
            arrive(seq + 1, 300, true);
            seq += 5;
            continue;
        }
        arrive(seq + 2, 100, false); // gap seq + 1
        arrive(seq + 1, 300, round != late_at);
        seq += 3;
    }
    heartbeat(1, seq + 2, false, 100); // a final tail
    advance_us(5000);
    for (uint32_t tail = seq; tail <= seq + 2; tail++) {
        arrive(tail, 50, true);
    }
    advance_us(5000);
}

static uint64_t script_log(int arm, uint8_t reveal, int late_at, uint8_t* copy, size_t* copy_len) {
    reset();
    set_arm(arm);
    run_script(reveal, late_at);
    EXPECT_TRUE(!send_log_overflow);
    if (copy != NULL) {
        memcpy(copy, send_log, send_log_len);
        *copy_len = send_log_len;
    }
    return fnv1a(send_log, send_log_len);
}

static uint8_t off_log[SEND_LOG_BYTES];

#ifdef tt_REORDER_HOLDOFF_OFF
// Printed by this file built against main ad4f7fe8's src/tickle.c (its MAIN_DIGEST lines), per reveal mode.
#define MAIN_DIGEST_REVEAL_OFF 0x92b9c432d09b0640ULL
#define MAIN_DIGEST_REVEALED 0x0f4483eaefe1c8c2ULL
#define MAIN_DIGEST_ALL_MISSING 0x0f4483eaefe1c8c2ULL
static const uint8_t reveals[3] = {tt_REPAIR_REVEAL_OFF, tt_REPAIR_REVEAL_REVEALED, tt_REPAIR_REVEAL_ALL_MISSING};

// The off arm's digests on main (ad4f7fe8), printed by this file built against main's src/tickle.c, per reveal mode.
static const uint64_t main_digest[3] = {MAIN_DIGEST_REVEAL_OFF, MAIN_DIGEST_REVEALED, MAIN_DIGEST_ALL_MISSING};

// (i): on a stream that never reorders every auto candidate sends exactly what off sends, and off what main sent.
static void test_without_reorder_auto_sends_what_off_sends(void) {
    for (int r = 0; r < 3; r++) {
        size_t off_len = 0;
        uint64_t off = script_log(ARM_OFF, reveals[r], -1, off_log, &off_len);
        EXPECT_TRUE(off_len > 0);
        EXPECT_EQ_U64(main_digest[r], off);
        for (int arm = ARM_B; arm <= ARM_E; arm++) {
            (void)script_log(arm, reveals[r], -1, NULL, NULL);
            EXPECT_EQ_U64(off_len, send_log_len);
            EXPECT_TRUE(send_log_len == off_len && memcmp(send_log, off_log, off_len) == 0);
            struct tt_ReliableStats stats;
            tt_reliable_stats_get(&stats);
            EXPECT_EQ_U64(0, stats.holdoff_engaged);     // nothing was held
            EXPECT_EQ_U64(0, stats.holdoff_gate_opened); // the gate never opened
            EXPECT_EQ_U64(0, stats.nack_spurious);       // the precondition: no late original
            EXPECT_TRUE(stats.acknack_immediate > 0 && stats.acknack_new_gap > 0 && stats.acknack_timer > 0 &&
                        stats.ack_request_answered + stats.ack_request_in_flight > 0);
            if (reveals[r] != tt_REPAIR_REVEAL_OFF) {
                EXPECT_TRUE(stats.acknack_reveal > 0);
            }
        }
        // The control: a hold the log can see - a fixed hold sends differently on the same script.
        (void)script_log(ARM_FIXED_1MS, reveals[r], -1, NULL, NULL);
        EXPECT_TRUE(send_log_len != off_len || memcmp(send_log, off_log, off_len) != 0);
    }
}

// The same script with one late original (round 20's lost datagram arrives as itself, after its NACK): the gate opens,
// later gaps are held, and every auto candidate sends differently from off - the comparison above is not blind to
// auto, auto is quiet there because nothing reordered.
static void test_one_late_original_opens_the_gate(void) {
    size_t off_len = 0;
    (void)script_log(ARM_OFF, tt_REPAIR_REVEAL_OFF, 20, off_log, &off_len);
    for (int arm = ARM_B; arm <= ARM_E; arm++) {
        (void)script_log(arm, tt_REPAIR_REVEAL_OFF, 20, NULL, NULL);
        struct tt_ReliableStats stats;
        tt_reliable_stats_get(&stats);
        EXPECT_EQ_U64(1, stats.holdoff_gate_opened);
        EXPECT_TRUE(stats.holdoff_engaged > 0);
        EXPECT_TRUE(send_log_len != off_len || memcmp(send_log, off_log, off_len) != 0);
    }
}

static struct tt_ReliableStats stats_now(void) {
    struct tt_ReliableStats stats;
    tt_reliable_stats_get(&stats);
    return stats;
}

// A held gap its late original fills is never NACKed, nor resent; off NACKs it at once and the original then shows
// the NACK spurious (the control).
static void test_a_held_gap_filled_late_is_never_nacked(void) {
    for (int arm = 0; arm < 2; arm++) {
        bool hold = arm == 1;
        reset();
        set_arm(hold ? ARM_FIXED_1MS : ARM_OFF);
        arrive(1, 50, false);
        arrive(3, 50, false); // gap 2
        EXPECT_EQ_INT(hold ? 0 : 1, test_mock_send_call_count);
        struct tt_WriterProxy* proxy = proxy_of();
        EXPECT_TRUE(proxy != NULL);
        EXPECT_EQ_U32(hold ? 1U : 0U, proxy->held_count);
        EXPECT_TRUE(proxy->holdoff_scheduled == hold);
        arrive(2, 300, false); // its original, 300 us late
        advance_us(3000);
        EXPECT_EQ_INT(hold ? 0 : 1, test_mock_send_call_count);
        struct tt_ReliableStats stats = stats_now();
        EXPECT_EQ_U64(hold ? 1U : 0U, stats.reorder_absorbed);
        EXPECT_EQ_U64(hold ? 0U : 1U, stats.nack_spurious);
        EXPECT_EQ_U64(hold ? 1U : 0U, stats.holdoff_engaged);
        EXPECT_EQ_U64(0, stats.holdoff_released);
        EXPECT_EQ_U32(0, proxy->held_count);
        EXPECT_TRUE(!proxy->holdoff_scheduled);
        EXPECT_TRUE(!proxy->acknack_scheduled);
        EXPECT_EQ_U32(4, proxy->ack_seq_no);
    }
}

// A held gap that stays open is NACKed when its hold has passed - not a microsecond before - as its first request,
// with the retry armed behind it.
static void test_a_held_gap_still_open_is_nacked_when_its_hold_has_passed(void) {
    reset();
    set_arm(ARM_FIXED_1MS);
    arrive(1, 50, false);
    arrive(3, 50, false);  // gap 2 shows now
    arrive(4, 999, false); // an arrival 1 us before the hold passes looks at the gap too: still held
    EXPECT_EQ_INT(0, test_mock_send_call_count);
    advance_us(1);
    EXPECT_EQ_INT(1, test_mock_send_call_count);
    EXPECT_TRUE(last_acknack_names(2));
    struct tt_WriterProxy* proxy = proxy_of();
    EXPECT_TRUE(proxy != NULL && proxy->acknack_scheduled && proxy->held_count == 0);
    struct tt_ReliableStats stats = stats_now();
    EXPECT_EQ_U64(1, stats.acknack_held);
    EXPECT_EQ_U64(1, stats.holdoff_released);
    EXPECT_EQ_U64(0, stats.holdoff_early);
    EXPECT_EQ_U64(1, stats.holdoff_value_hist[10]); // 1 ms: [512, 1024) us
}

// A gap opened while the retry runs is held too: no new-gap ACKNACK (N2), and the retry leaves it out (N3) until its
// own hold has passed, when it alone is NACKed.
static void test_a_gap_opened_while_the_retry_runs_is_held_from_it(void) {
    reset();
    set_arm(ARM_FIXED_1MS);
    arrive(1, 50, false);
    arrive(3, 50, false); // gap 2 at t0
    advance_us(1000);     // released: NACK 2, retry due at t0 + 2 ms
    EXPECT_EQ_INT(1, test_mock_send_call_count);
    arrive(5, 100, false); // gap 4 at t0 + 1.1 ms
    EXPECT_EQ_INT(1, test_mock_send_call_count);
    advance_us(900); // t0 + 2 ms: the retry
    EXPECT_EQ_INT(2, test_mock_send_call_count);
    EXPECT_TRUE(last_acknack_names(2));
    EXPECT_TRUE(!last_acknack_names(4));
    advance_us(100); // t0 + 2.1 ms: gap 4's hold has passed
    EXPECT_EQ_INT(3, test_mock_send_call_count);
    EXPECT_TRUE(last_acknack_names(4));
    EXPECT_TRUE(!last_acknack_names(2));
    EXPECT_EQ_U64(2, stats_now().acknack_held);
}

// A gap the highest received has passed by half the window is released at once, whatever its hold (the window would
// otherwise move past it and abandon it): the early rule. The gap opened by that same arrival stays held.
static void test_a_gap_half_a_window_behind_is_released_early(void) {
    reset();
    set_arm(ARM_FIXED_1MS);
    node.holdoff_ns = (uint32_t)tt_REORDER_HOLDOFF_MAX; // a hold nothing but the rule can end
    arrive(1, 50, false);
    arrive(3, 50, false); // gap 2
    struct tt_WriterProxy* proxy = proxy_of();
    uint32_t half = proxy_window_bits(proxy) / 2U;
    arrive(3 + half - 2U, 50, false); // 2 lies half - 1 behind: still held
    EXPECT_EQ_INT(0, test_mock_send_call_count);
    arrive(3 + half - 1U, 50, false); // ...and now half behind
    EXPECT_EQ_INT(1, test_mock_send_call_count);
    EXPECT_TRUE(last_acknack_names(2));
    EXPECT_TRUE(!last_acknack_names(3 + half - 3U)); // the newer gap: held
    struct tt_ReliableStats stats = stats_now();
    EXPECT_EQ_U64(1, stats.holdoff_early);
    EXPECT_EQ_U64(1, stats.holdoff_released);
}

// Forces auto's gate open on this proxy, as an E1 would, with the reorder delay seen `delay_us`.
static void open_gate(uint32_t delay_us) {
    struct tt_WriterProxy* proxy = proxy_of();
    EXPECT_TRUE(proxy != NULL);
    proxy->holdoff_gate = true;
    proxy->holdoff_m = 1;
    proxy->holdoff_delay_ns = delay_us * (uint32_t)tt_MICROSECOND;
}

// B: released once HOLDOFF_THR_MIN datagrams arrived past the gap, long before its backstop (the retry interval, 1 ms
// before any estimate); two are not enough.
static void test_b_releases_on_distance(void) {
    reset();
    set_arm(ARM_B);
    arrive(1, 50, false);
    open_gate(100);
    arrive(3, 10, false); // gap 2
    arrive(4, 10, false); // 2 behind
    EXPECT_EQ_INT(0, test_mock_send_call_count);
    arrive(5, 10, false); // 3 behind
    EXPECT_EQ_INT(1, test_mock_send_call_count);
    EXPECT_TRUE(last_acknack_names(2));
    EXPECT_EQ_U64(0, stats_now().holdoff_early);
}

// D: with the distance there, released at a quarter of E's time; without it (the control), at E's time.
static void test_d_releases_on_distance_after_a_quarter(void) {
    for (int arm = 0; arm < 2; arm++) {
        bool far = arm == 0;
        reset();
        set_arm(ARM_D);
        arrive(1, 50, false);
        open_gate(800);       // E's time: m x D = 800 us (no estimate: the cap is max(retry_initial, D) = 1 ms)
        arrive(3, 10, false); // gap 2 at t0
        if (far) {
            arrive(4, 10, false);
            arrive(5, 10, false); // 3 behind at t0 + 20 us
            advance_us(179);      // t0 + 199 us
        } else {
            advance_us(199);
        }
        EXPECT_EQ_INT(0, test_mock_send_call_count);
        advance_us(1); // t0 + 200 us: a quarter
        EXPECT_EQ_INT(far ? 1 : 0, test_mock_send_call_count);
        advance_us(600); // t0 + 800 us
        EXPECT_EQ_INT(1, test_mock_send_call_count);
        EXPECT_TRUE(last_acknack_names(2));
    }
}

// A HEARTBEAT's tail is held as a gap (N4); off NACKs it at once (the control). And a HEARTBEAT asking for an answer
// while gaps are held gets one that leaves them out.
static void test_a_heartbeat_tail_is_held(void) {
    for (int arm = 0; arm < 2; arm++) {
        bool hold = arm == 1;
        reset();
        set_arm(hold ? ARM_FIXED_1MS : ARM_OFF);
        arrive(1, 50, false);
        arrive(2, 50, false);
        heartbeat(1, 5, true, 50); // 3-5 announced, not received
        EXPECT_EQ_INT(hold ? 0 : 1, test_mock_send_call_count);
        if (!hold) {
            EXPECT_TRUE(last_acknack_names(3) && last_acknack_names(5));
        }
        advance_us(1000);
        EXPECT_TRUE(test_mock_send_call_count >= 1);
        EXPECT_TRUE(last_acknack_names(3) && last_acknack_names(5));
    }
    reset();
    set_arm(ARM_FIXED_1MS);
    arrive(1, 50, false);
    arrive(3, 50, false);
    advance_us(1000); // gap 2 NACKed at t0, the retry armed for t0 + 1 ms
    EXPECT_EQ_INT(1, test_mock_send_call_count);
    heartbeat(1, 6, false, 100); // 4-6 revealed and held; the request is answered without them
    EXPECT_EQ_INT(2, test_mock_send_call_count);
    EXPECT_TRUE(last_acknack() != NULL);
    EXPECT_TRUE(!last_acknack_names(4) && !last_acknack_names(6));
    advance_us(1000); // the retry at t0 + 1 ms (2 only), the tail's release at t0 + 1.1 ms
    EXPECT_EQ_INT(4, test_mock_send_call_count);
    EXPECT_TRUE(last_acknack_names(4) && last_acknack_names(6) && !last_acknack_names(2));
}

// Auto (E): the gate opens on the first late original after its NACK (E1) - learning its delay - after which a gap is
// held; HOLDOFF_GATE_CLOSE clean recoveries later it closes, and a gap is NACKed at once again. One fewer leaves it
// open.
static void test_auto_gate_opens_on_a_spurious_nack_and_closes_after_clean_recoveries(void) {
    reset();
    set_arm(ARM_E);
    arrive(1, 50, false);
    arrive(3, 50, false); // gap 2: the gate is closed, NACKed at once
    EXPECT_EQ_INT(1, test_mock_send_call_count);
    struct tt_WriterProxy* proxy = proxy_of();
    EXPECT_TRUE(!proxy->holdoff_gate);
    arrive(2, 300, false); // its original, late: E1
    EXPECT_TRUE(proxy->holdoff_gate);
    EXPECT_EQ_U64(1, stats_now().holdoff_gate_opened);
    EXPECT_EQ_U32(300U * (uint32_t)tt_MICROSECOND, proxy->holdoff_delay_ns);
    uint32_t seq = 4;
    for (uint32_t round = 0; round < HOLDOFF_GATE_CLOSE; round++) {
        EXPECT_TRUE(proxy->holdoff_gate);
        int before = test_mock_send_call_count;
        arrive(seq, 50, false);
        arrive(seq + 2, 50, false); // a gap: held while the gate is open
        EXPECT_EQ_INT(before, test_mock_send_call_count);
        advance_us(2000); // released and NACKed (and maybe retried)
        EXPECT_TRUE(test_mock_send_call_count > before);
        arrive(seq + 1, 100, true); // its repair: a clean recovery
        seq += 3;
    }
    EXPECT_TRUE(!proxy->holdoff_gate);
    int before = test_mock_send_call_count;
    arrive(seq, 50, false);
    arrive(seq + 2, 50, false);
    EXPECT_EQ_INT(before + 1, test_mock_send_call_count); // NACKed at once again
}

// The options: profile defaults, refusals, the context's fold over its links, and the A/B seam.
static void test_options_resolve_fold_and_refuse(void) {
    struct tt_LinkOptions link;
    const uint8_t profiles[4] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_WIFI, tt_LINK_PROFILE_T1S,
                                 tt_LINK_PROFILE_TSN};
    const uint64_t expected[4] = {(uint64_t)tt_RELIABLE_REORDER_HOLDOFF, (uint64_t)tt_RELIABLE_REORDER_HOLDOFF,
                                  tt_REORDER_HOLDOFF_OFF, tt_REORDER_HOLDOFF_OFF};
    for (int i = 0; i < 4; i++) {
        memset(&link, 0, sizeof(link));
        link.profile = profiles[i];
        resolve_link_repair_timing(&link);
        EXPECT_EQ_U64(expected[i], link.reorder_holdoff);
        link.reorder_holdoff = 5 * tt_MILLISECOND; // a set value is the link's own
        resolve_link_repair_timing(&link);
        EXPECT_EQ_U64(5 * tt_MILLISECOND, link.reorder_holdoff);
    }
    EXPECT_EQ_U64(0, (uint64_t)tt_RELIABLE_REORDER_HOLDOFF); // the build's default is auto

    memset(&link, 0, sizeof(link));
    link.link_rate = tt_LINK_RATE_MIN;
    link.pacing_rate = tt_PACING_OFF;
    link.reorder_holdoff = tt_REORDER_HOLDOFF_MAX;
    EXPECT_TRUE(validate_link_profile(&link) == tt_RET_OK);
    link.reorder_holdoff = tt_REORDER_HOLDOFF_OFF;
    EXPECT_TRUE(validate_link_profile(&link) == tt_RET_OK);
    link.reorder_holdoff = tt_REORDER_HOLDOFF_MAX + 1U;
    EXPECT_TRUE(validate_link_profile(&link) == tt_RET_INVALID_ARGUMENT);

    struct tt_ReliableOptions reliable;
    memset(&reliable, 0, sizeof(reliable));
    resolve_reliable_options(&reliable);
    EXPECT_EQ_U64(0, reliable.reorder_holdoff);
    EXPECT_TRUE(validate_reliable_options(&reliable) == tt_RET_OK);
    reliable.reorder_holdoff = tt_REORDER_HOLDOFF_MAX + 1U;
    EXPECT_TRUE(validate_reliable_options(&reliable) == tt_RET_INVALID_ARGUMENT);
    reliable.reorder_holdoff = tt_REORDER_HOLDOFF_OFF;
    EXPECT_TRUE(validate_reliable_options(&reliable) == tt_RET_OK);

    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.link_count = 1;
    opts.net.links[0].reorder_holdoff = 2 * tt_MILLISECOND;
    EXPECT_TRUE(validate_link_conflicts(&opts) == tt_RET_OK); // the context left it to the links
    opts.reliable.reorder_holdoff = 2 * tt_MILLISECOND;
    EXPECT_TRUE(validate_link_conflicts(&opts) == tt_RET_OK); // the same answer twice
    opts.reliable.reorder_holdoff = 3 * tt_MILLISECOND;
    EXPECT_TRUE(validate_link_conflicts(&opts) == tt_RET_INVALID_ARGUMENT);

    struct tt_Capacity capacity;
    memset(&capacity, 0, sizeof(capacity));
    resolve_capacity_options(&capacity);
    EXPECT_EQ_U32(tt_RELIABLE_HOLDOFF_SLOTS, capacity.holdoff_slots);
    struct tt_NetOptions net;
    memset(&net, 0, sizeof(net));
    EXPECT_TRUE(validate_capacity_options(&capacity, &net) == tt_RET_OK);
    capacity.holdoff_slots = tt_RELIABLE_HOLDOFF_SLOTS + 1U;
    EXPECT_TRUE(validate_capacity_options(&capacity, &net) == tt_RET_INVALID_ARGUMENT);

    // The fold: auto if any link is, else the longest fixed hold, off only if every link is; the context's own wins.
    reset();
    const uint64_t any_auto[3] = {tt_REORDER_HOLDOFF_OFF, 2 * tt_MILLISECOND, tt_REORDER_HOLDOFF_AUTO};
    holdoff_apply(&node, any_auto, 3);
    EXPECT_EQ_U32(tt_HOLDOFF_MODE_AUTO, node.holdoff_mode);
    EXPECT_EQ_U32(tt_HOLDOFF_POLICY_E, node.holdoff_policy);
    const uint64_t fixed[3] = {tt_REORDER_HOLDOFF_OFF, 5 * tt_MILLISECOND, 2 * tt_MILLISECOND};
    holdoff_apply(&node, fixed, 3);
    EXPECT_EQ_U32(tt_HOLDOFF_MODE_FIXED, node.holdoff_mode);
    EXPECT_EQ_U32(5U * (uint32_t)tt_MILLISECOND, node.holdoff_ns);
    const uint64_t all_off[2] = {tt_REORDER_HOLDOFF_OFF, tt_REORDER_HOLDOFF_OFF};
    holdoff_apply(&node, all_off, 2);
    EXPECT_EQ_U32(tt_HOLDOFF_MODE_OFF, node.holdoff_mode);
    node.opts.reliable.reorder_holdoff = 7 * tt_MILLISECOND;
    holdoff_apply(&node, any_auto, 3);
    EXPECT_EQ_U32(tt_HOLDOFF_MODE_FIXED, node.holdoff_mode);
    EXPECT_EQ_U32(7U * (uint32_t)tt_MILLISECOND, node.holdoff_ns);
    node.opts.reliable.reorder_holdoff = tt_REORDER_HOLDOFF_OFF;
    holdoff_apply(&node, any_auto, 3);
    EXPECT_EQ_U32(tt_HOLDOFF_MODE_OFF, node.holdoff_mode);
    node.opts.reliable.reorder_holdoff = 0;

    // The seam: a value replaces the result, a policy picks the candidate.
    tt_holdoff_test_value = tt_REORDER_HOLDOFF_OFF;
    holdoff_apply(&node, any_auto, 3);
    EXPECT_EQ_U32(tt_HOLDOFF_MODE_OFF, node.holdoff_mode);
    tt_holdoff_test_value = 4 * tt_MILLISECOND;
    holdoff_apply(&node, all_off, 2);
    EXPECT_EQ_U32(tt_HOLDOFF_MODE_FIXED, node.holdoff_mode);
    EXPECT_EQ_U32(4U * (uint32_t)tt_MILLISECOND, node.holdoff_ns);
    tt_holdoff_test_value = 0;
    tt_holdoff_test_policy = tt_HOLDOFF_POLICY_B;
    holdoff_apply(&node, any_auto, 3);
    EXPECT_EQ_U32(tt_HOLDOFF_POLICY_B, node.holdoff_policy);
    tt_holdoff_test_policy = tt_HOLDOFF_POLICY_DEFAULT;
}

static void test_holdoff_mechanisms(void) {
    test_a_held_gap_filled_late_is_never_nacked();
    test_a_held_gap_still_open_is_nacked_when_its_hold_has_passed();
    test_a_gap_opened_while_the_retry_runs_is_held_from_it();
    test_a_gap_half_a_window_behind_is_released_early();
    test_b_releases_on_distance();
    test_d_releases_on_distance_after_a_quarter();
    test_a_heartbeat_tail_is_held();
    test_auto_gate_opens_on_a_spurious_nack_and_closes_after_clean_recoveries();
    test_options_resolve_fold_and_refuse();
}
#endif

int main(void) {
#ifndef tt_REORDER_HOLDOFF_OFF
    // Built against a tree without the hold-off (main): print the off arm's digests for main_digest[].
    const uint8_t modes[3] = {tt_REPAIR_REVEAL_OFF, tt_REPAIR_REVEAL_REVEALED, tt_REPAIR_REVEAL_ALL_MISSING};
    for (int r = 0; r < 3; r++) {
        uint64_t digest = script_log(ARM_OFF, modes[r], -1, NULL, NULL);
        printf("MAIN_DIGEST reveal=%u 0x%016llxULL len=%zu\n", (unsigned)modes[r], (unsigned long long)digest,
               send_log_len);
    }
    return test_result();
#else
    test_without_reorder_auto_sends_what_off_sends();
    test_one_late_original_opens_the_gate();
    test_holdoff_mechanisms();
    printf("test_reorder_holdoff: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
#endif
}
