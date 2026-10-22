/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Send pacing (tt_LinkOptions.pacing_rate and pacing_burst; docs/DESIGN.md "Send pacing"), through the mock HAL and
// tt_Context_create_with(), built as rmw_tickle builds core (a 65507-byte datagram, large samples):
//   - ETHERNET, TSN, WIFI and a context with no links configured pace nothing, and count nothing into a bucket;
//   - T1S paces at tt_LINK_T1S_PACING_PCT percent of its link_rate with a burst of tt_PACING_BURST_FRAMES frames;
//   - a large sample on a paced link leaves a burst at a time, from the scheduler, at the pacing rate, and the
//     publish returns without waiting; unpaced, it all goes in the publish (the control);
//   - every field changes what goes when: pacing_rate (the time the sample takes), pacing_burst (what goes at once),
//     link_rate (T1S's default rate), tt_PACING_OFF (none); ordinary sends are counted into the bucket and delay a
//     large sample behind them; a retransmission goes while the backlog is within two bursts and is left to the
//     reader past that;
//   - each check refuses the value past its bound with tt_RET_INVALID_ARGUMENT and accepts the bound itself
//     (tests/mutants_send_pacing.py removes each check and each reader and expects a test here to fail).

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1 // as rmw_tickle builds core

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_send_pacing.py passes a mutant

#define LINK_BROADCAST "10.0.0.255"
#define PEER_IP 0x0a000009U
#define PEER_ID 9
#define PORT 8282
#define LARGE_LEN (256U * 1024U) // a large sample: above tt_MAX_SAMPLE_LENGTH
#define T1S_RATE 10000000ULL
#define T1S_PACED (T1S_RATE / 100U * tt_LINK_T1S_PACING_PCT)
#define BURST_1500 (tt_PACING_BURST_FRAMES * (1500U + 38U))

static struct tt_Context node;
static struct tt_TypeSupport topic;
static struct tt_Publisher pub;
static uint64_t pub_storage[(size_t)256U * 1024U / sizeof(uint64_t)];
static uint32_t sample_len;

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    memset(payload, 0x5A, len);
    return (int32_t)len;
}

static uint32_t buffers_out;

static void* test_acquire(void* user, uint32_t bytes) {
    (void)user;
    buffers_out++;
    return malloc(bytes);
}

static void test_release(void* user, void* buffer) {
    (void)user;
    buffers_out--;
    free(buffer);
}

// Every datagram the HAL is handed: when (the mock clock), how long, and whether it was a large sample's unit.
#define SENT_MAX 4096U
static struct {
    uint64_t at;
    uint32_t len;
    bool large;
} sent[SENT_MAX];
static uint32_t sent_count;

// A datagram's first submessage type, in either header form.
static uint8_t datagram_type(const uint8_t* bytes) {
    if (bytes[0] == tt_SINGLE_MARKER_LE || bytes[0] == tt_SINGLE_MARKER_BE) {
        return ((const struct tt_SingleHeader*)bytes)->type;
    }
    return ((const struct tt_SubmessageHeader*)(bytes + sizeof(struct tt_Header)))->type;
}

static void capture(const void* buf, size_t len) {
    if (sent_count < SENT_MAX) {
        uint8_t type = datagram_type((const uint8_t*)buf);
        sent[sent_count].at = test_mock_now;
        sent[sent_count].len = (uint32_t)len;
        sent[sent_count].large = type == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L || type == tt_SUBMESSAGE_TYPE_FRAG_CONT_L;
    }
    sent_count++;
}

static struct tt_InitOptions one_link(uint8_t profile) {
    struct tt_InitOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.net.links[0] = (struct tt_LinkOptions) {.broadcast = LINK_BROADCAST, .profile = profile};
    opts.net.link_count = 1;
    return opts;
}

static void create(const struct tt_InitOptions* opts) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    test_mock_link_mtu = -1; // unknown: a T1S link takes 1500
    memset(&node, 0, sizeof(node));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&node, opts));
    memset(&topic, 0, sizeof(topic));
    topic.name = "pacing_topic";
    topic.data_size = 8;
    topic.data_encode_size = sized_encode_size;
    topic.data_encode = sized_encode;
}

// A Publisher with one known reader, large buffers, and `qos`; never batching (T1S's default), so that every publish
// sends.
static void create_publisher(const struct tt_QosProfile* qos) {
    size_t storage = tt_Publisher_storage_size(&topic, qos);
    EXPECT_TRUE(storage <= sizeof(pub_storage));
    struct tt_PublisherOptions unbatched = {.batching = tt_SWITCH_OFF};
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node), &pub, &topic, "pacing", qos,
                                                      &unbatched, storage != 0 ? pub_storage : NULL, storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&node, test_acquire, test_release, NULL));
    (void)upsert_peer(pub.peers, PEER_ID, PEER_IP, PORT);
}

static void destroy(void) {
    test_mock_send_hook = NULL;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&node));
    EXPECT_EQ_U32(0, buffers_out);
}

static bool large_unsent(void) {
    for (uint32_t i = 0; i < pub.large_count; i++) {
        if (!large_record_at(&pub, i)->sent) {
            return true;
        }
    }
    return false;
}

// Runs the context's scheduler on the mock clock, jumping to each entry's time, until the large sample has gone (or a
// minute of mock time, or a million entries, have passed: a send that reschedules itself for the same instant ends
// here).
static void run_until_sent(void) {
    uint64_t give_up = test_mock_now + (60ULL * tt_SECOND);
    uint32_t entries = 0;
    while (large_unsent() && test_mock_now < give_up && entries < 1000000U) {
        uint64_t next = 0;
        bool has_next = false;
        if (!sched_next_time(&node, &next)) {
            break;
        }
        test_mock_now = next > test_mock_now ? next : test_mock_now;
        while (entries < 1000000U && run_due_entry(&node, test_mock_now, &has_next, &next)) {
            entries++;
        }
    }
}

struct large_send {
    uint32_t units;          // the large sample's datagrams
    uint32_t in_publish;     // of them, sent before the publish returned
    uint32_t most_at_once;   // the most sent at one instant of the mock clock
    uint32_t steady_at_once; // the same after the first instant
    uint64_t first_at;       // when the first went
    uint64_t span_ns;        // from the first to the last
    uint64_t wire_after_one; // bytes on the wire (pace_cost_ns()'s count) of every unit after the first instant's
};

// The IP payload a frame of the link carries, for counting a unit's bytes on the wire independently of the pacer's own
// count (1480 at an MTU of 1500).
static uint32_t frame_ip_payload = 1480;

// Publishes one large sample of LARGE_LEN and lets it go out; what its units did.
static struct large_send send_large(void) {
    struct large_send result = {0};
    sample_len = LARGE_LEN;
    sent_count = 0;
    test_mock_send_hook = capture;
    uint8_t data[8] = {0};
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    uint32_t before_return = sent_count;
    run_until_sent();
    EXPECT_TRUE(!large_unsent());
    EXPECT_TRUE(sent_count <= SENT_MAX);
    bool first = true;
    uint64_t last = 0;
    uint32_t at_once = 0;
    uint64_t instant = 0;
    for (uint32_t i = 0; i < sent_count && i < SENT_MAX; i++) {
        if (!sent[i].large) {
            continue;
        }
        result.units++;
        result.in_publish += i < before_return ? 1U : 0U;
        if (first) {
            result.first_at = sent[i].at;
            instant = sent[i].at;
            first = false;
        }
        at_once = sent[i].at == instant ? at_once + 1U : 1U;
        instant = sent[i].at;
        result.most_at_once = at_once > result.most_at_once ? at_once : result.most_at_once;
        if (sent[i].at != result.first_at) {
            result.steady_at_once = at_once > result.steady_at_once ? at_once : result.steady_at_once;
            uint32_t ip_payload = sent[i].len + 8U;
            uint32_t frames = 1U + ((ip_payload - 1U) / frame_ip_payload);
            result.wire_after_one += ip_payload + (frames * 58U);
        }
        last = sent[i].at;
    }
    result.span_ns = last - result.first_at;
    return result;
}

// The rate the units after the first instant went at, in bits per second.
static uint64_t measured_rate(const struct large_send* s) {
    return s->span_ns != 0 ? s->wire_after_one * 8U * tt_SECOND / s->span_ns : 0U;
}

static bool within_pct(uint64_t value, uint64_t expected, uint64_t pct) {
    uint64_t slack = expected * pct / 100U;
    return value + slack >= expected && value <= expected + slack;
}

// ---- Defaults: only T1S paces.

static void test_only_t1s_paces_by_default(void) {
    // A hand-built context.
    memset(&node, 0, sizeof(node));
    test_mock_reset();
    node_init_locks(&node);
    EXPECT_TRUE(!node.pacing);

    create(NULL);
    EXPECT_TRUE(!node.pacing);
    EXPECT_EQ_U64(0, node.pacers[0].ns_per_byte_q16);
    destroy();

    static const uint8_t unpaced[] = {tt_LINK_PROFILE_ETHERNET, tt_LINK_PROFILE_TSN, tt_LINK_PROFILE_WIFI};
    for (size_t i = 0; i < sizeof(unpaced); i++) {
        struct tt_InitOptions opts = one_link(unpaced[i]);
        create(&opts);
        EXPECT_TRUE(!node.pacing);
        EXPECT_EQ_U64(tt_PACING_OFF, node.opts.net.links[0].pacing_rate);
        EXPECT_EQ_U32(0, node.opts.net.links[0].pacing_burst);
        EXPECT_EQ_U64(0, node.pacers[0].ns_per_byte_q16);
        // A send is counted into no bucket.
        create_publisher(&TEST_QOS_UNRELIABLE);
        struct large_send s = send_large();
        EXPECT_EQ_U64(0, node.pacers[0].until_ns);
        EXPECT_EQ_U32(s.units, s.in_publish); // all of it in the publish
        destroy();
    }

    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_T1S);
    create(&opts);
    EXPECT_TRUE(node.pacing);
    EXPECT_EQ_U64(T1S_PACED, node.opts.net.links[0].pacing_rate);
    EXPECT_EQ_U32(BURST_1500, node.opts.net.links[0].pacing_burst);
    EXPECT_EQ_U64(((8ULL * tt_SECOND) << 16U) / T1S_PACED, node.pacers[0].ns_per_byte_q16);
    EXPECT_EQ_U32(1480, node.pacers[0].frame_payload);
    // 6152 bytes at 7.5 Mbit/s: 6562 us.
    EXPECT_TRUE(within_pct(node.pacers[0].burst_ns, (uint64_t)BURST_1500 * 8U * tt_SECOND / T1S_PACED, 1));
    destroy();

    // The burst follows the link MTU (1200: the least at which this build still fragments its largest small sample).
    opts.net.links[0].mtu = 1200;
    create(&opts);
    EXPECT_EQ_U32(tt_PACING_BURST_FRAMES * (1200U + 38U), node.opts.net.links[0].pacing_burst);
    EXPECT_EQ_U32(1176, node.pacers[0].frame_payload);
    destroy();
}

// ---- A large sample on a paced link: a burst at a time from the scheduler, at the rate.

static void test_t1s_paces_a_large_sample(void) {
    // The control: T1S with pacing off sends the whole sample in the publish.
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].pacing_rate = tt_PACING_OFF;
    create(&opts);
    EXPECT_TRUE(!node.pacing);
    create_publisher(&TEST_QOS_UNRELIABLE);
    struct large_send off = send_large();
    EXPECT_TRUE(off.units > 150); // 256 KB in units of ~1.4 KB
    EXPECT_EQ_U32(off.units, off.in_publish);
    EXPECT_EQ_U64(0, off.span_ns);
    EXPECT_EQ_U64(0, node.pacing_waits);
    destroy();

    opts = one_link(tt_LINK_PROFILE_T1S);
    create(&opts);
    create_publisher(&TEST_QOS_UNRELIABLE);
    struct large_send on = send_large();
    printf("test_send_pacing: T1S 256 KB: %u units, %u in the publish, at most %u at once, %llu us, %llu bit/s, "
           "%llu waits\n",
           on.units, on.in_publish, on.most_at_once, (unsigned long long)(on.span_ns / tt_MICROSECOND),
           (unsigned long long)measured_rate(&on), (unsigned long long)node.pacing_waits);
    EXPECT_EQ_U32(off.units, on.units);
    EXPECT_TRUE(on.in_publish <= 4); // a burst: 4 frames of 1500
    EXPECT_TRUE(on.most_at_once <= 4);
    EXPECT_EQ_U32(1, on.steady_at_once); // then one at a time, as the bucket refills by one unit
    EXPECT_TRUE(within_pct(measured_rate(&on), T1S_PACED, 3));
    EXPECT_TRUE(node.pacing_waits >= on.units / 4U - 1U);
    destroy();
}

// ---- Each field changes what goes when.

static void test_each_field_changes_the_send(void) {
    // pacing_rate: half the rate, twice the time.
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].pacing_rate = T1S_PACED / 2U;
    create(&opts);
    create_publisher(&TEST_QOS_UNRELIABLE);
    struct large_send half = send_large();
    EXPECT_TRUE(within_pct(measured_rate(&half), T1S_PACED / 2U, 3));
    destroy();

    // link_rate: T1S's default rate follows it.
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].link_rate = 2U * T1S_RATE;
    create(&opts);
    EXPECT_EQ_U64(2U * T1S_PACED, node.opts.net.links[0].pacing_rate);
    create_publisher(&TEST_QOS_UNRELIABLE);
    struct large_send fast = send_large();
    EXPECT_TRUE(within_pct(measured_rate(&fast), 2U * T1S_PACED, 3));
    destroy();

    // pacing_burst: more at once.
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].pacing_burst = 16000;
    create(&opts);
    create_publisher(&TEST_QOS_UNRELIABLE);
    struct large_send deep = send_large();
    EXPECT_TRUE(deep.most_at_once >= 9 && deep.most_at_once <= 10); // 16000 / 1538 bytes a unit
    EXPECT_TRUE(within_pct(measured_rate(&deep), T1S_PACED, 3));
    destroy();

    // An ETHERNET link given a rate is paced; its default burst is four frames of its MTU.
    opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[0].pacing_rate = 100000000ULL;
    create(&opts);
    EXPECT_TRUE(node.pacing);
    EXPECT_EQ_U32(BURST_1500, node.opts.net.links[0].pacing_burst);
    create_publisher(&TEST_QOS_UNRELIABLE);
    struct large_send eth = send_large();
    EXPECT_TRUE(within_pct(measured_rate(&eth), 100000000ULL, 3));
    destroy();
}

// ---- A datagram longer than the whole burst goes alone, from a full bucket: at an MTU of 1200 a large unit is two
// frames (1,596 bytes on the wire), past a burst of tt_PACING_BURST_MIN.

static void test_a_unit_longer_than_the_burst(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].mtu = 1200;
    opts.net.links[0].pacing_burst = tt_PACING_BURST_MIN;
    create(&opts);
    create_publisher(&TEST_QOS_UNRELIABLE);
    frame_ip_payload = 1176;
    struct large_send s = send_large();
    EXPECT_TRUE(s.units > 150);
    EXPECT_EQ_U32(1, s.in_publish);
    EXPECT_EQ_U32(1, s.most_at_once);
    EXPECT_TRUE(within_pct(measured_rate(&s), T1S_PACED, 3)); // counted as two frames each, or it would run fast
    frame_ip_payload = 1480;
    destroy();
}

// ---- Two links: each its own bucket, a destination counted into its own link's.

static void test_two_links_two_buckets(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .profile = tt_LINK_PROFILE_T1S};
    opts.net.link_count = 2;
    create(&opts);
    EXPECT_TRUE(node.pacing);
    EXPECT_EQ_U64(0, node.pacers[0].ns_per_byte_q16);
    EXPECT_TRUE(node.pacers[1].ns_per_byte_q16 != 0);
    // The mock resolves every link to one subnet: link 1 is put on its own, as an interface of its own would be.
    node.links[1].resolved_addr = 0x0a000107U;
    node.links[1].resolved_broadcast = 0x0a0001ffU;
    create_publisher(&TEST_QOS_UNRELIABLE);
    pub.peers[0].ip = 0x0a000109U; // the one reader, on link 1: a T1S broadcast there
    sample_len = 1000;
    uint8_t data[8] = {0};
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    EXPECT_TRUE(node.pacers[1].until_ns > test_mock_now);
    EXPECT_EQ_U64(0, node.pacers[0].until_ns);
    // A reader on link 0 only: nothing counted into link 1's bucket.
    uint64_t until = node.pacers[1].until_ns;
    pub.peers[0].ip = PEER_IP;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    EXPECT_EQ_U64(until, node.pacers[1].until_ns);
    EXPECT_EQ_U64(0, node.pacers[0].until_ns);
    destroy();
}

// ---- Ordinary sends are counted, and a large sample waits behind them.

static void test_ordinary_sends_fill_the_bucket(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_T1S);
    create(&opts);
    create_publisher(&TEST_QOS_UNRELIABLE);
    test_mock_send_hook = capture;
    // A 1000-byte small sample goes at once, whatever the bucket says, and fills it by its bytes on the wire.
    sample_len = 1000;
    uint8_t data[8] = {0};
    sent_count = 0;
    uint64_t before = node.pacers[0].until_ns > test_mock_now ? node.pacers[0].until_ns - test_mock_now : 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    EXPECT_TRUE(sent_count >= 1);
    uint64_t after = node.pacers[0].until_ns - test_mock_now;
    uint64_t one = ((uint64_t)(1000U + 8U + 58U + 32U) * node.pacers[0].ns_per_byte_q16) >> 16U; // + DATA framing
    EXPECT_TRUE(after > before && after - before >= one - (one / 10U) && after - before <= one + (one / 10U));
    // Five more fill the bucket past a burst: the large sample's first unit waits for it.
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    }
    EXPECT_TRUE(node.pacers[0].until_ns - test_mock_now > node.pacers[0].burst_ns);
    uint64_t published_at = test_mock_now;
    struct large_send s = send_large();
    EXPECT_EQ_U32(0, s.in_publish);
    EXPECT_TRUE(s.first_at > published_at);
    destroy();

    // A paced ETHERNET link: a unicast to one reader (one datagram), then to two (one datagram each, in one batch).
    opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[0].pacing_rate = T1S_RATE;
    create(&opts);
    create_publisher(&TEST_QOS_UNRELIABLE);
    sample_len = 1000;
    uint64_t one_unicast = ((uint64_t)(1000U + 8U + 58U + 32U) * node.pacers[0].ns_per_byte_q16) >> 16U;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    uint64_t first = node.pacers[0].until_ns - test_mock_now;
    EXPECT_TRUE(first >= one_unicast - (one_unicast / 10U) && first <= one_unicast + (one_unicast / 10U));
    (void)upsert_peer(pub.peers, PEER_ID + 1, PEER_IP + 1U, PORT);
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    uint64_t two = node.pacers[0].until_ns - test_mock_now - first;
    EXPECT_TRUE(two >= 2U * (one_unicast - (one_unicast / 10U)) && two <= 2U * (one_unicast + (one_unicast / 10U)));
    destroy();
}

// ---- Retransmissions: within two bursts they go, past that they are left to the reader.

static void test_retransmissions_are_paced(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_T1S);
    create(&opts);
    struct tt_QosProfile reliable = {.history = {.kind = tt_QOS_HISTORY_KEEP_LAST, .depth = 2},
                                     .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE}};
    create_publisher(&reliable);
    sample_len = 200;
    uint8_t data[8] = {0};
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    struct tt_Peer target = {PEER_ID, PEER_IP, PORT};
    struct tt_ReliableCache* cache = pub.reliable_cache;
    EXPECT_TRUE(cache != NULL);
    uint16_t depth = reliable_cache_depth(cache);
    test_mock_send_hook = capture;

    // An empty bucket: the repair goes.
    node.pacers[0].until_ns = test_mock_now;
    sent_count = 0;
    EXPECT_TRUE(!retransmit_one_sample(&node, &pub, cache, depth, pub.seq_no, &target));
    EXPECT_EQ_U32(1, sent_count);
    EXPECT_EQ_U64(0, node.pacing_repairs_deferred);
    // A backlog of a burst and a half: still within two, it goes.
    node.pacers[0].until_ns = test_mock_now + node.pacers[0].burst_ns + (node.pacers[0].burst_ns / 2U);
    sent_count = 0;
    EXPECT_TRUE(!retransmit_one_sample(&node, &pub, cache, depth, pub.seq_no, &target));
    EXPECT_EQ_U32(1, sent_count);
    // Two bursts: it is left to the reader - not sent, not gone, counted.
    node.pacers[0].until_ns = test_mock_now + (2U * node.pacers[0].burst_ns);
    uint64_t until = node.pacers[0].until_ns;
    sent_count = 0;
    EXPECT_TRUE(!retransmit_one_sample(&node, &pub, cache, depth, pub.seq_no, &target));
    EXPECT_EQ_U32(0, sent_count);
    EXPECT_EQ_U64(1, node.pacing_repairs_deferred);
    EXPECT_EQ_U64(until, node.pacers[0].until_ns);

    // A large sample's unit, the same way.
    struct large_send s = send_large();
    EXPECT_TRUE(s.units > 0);
    const struct tt_LargeRecord* record = large_record_at(&pub, pub.large_count - 1U);
    node.pacers[0].until_ns = test_mock_now;
    sent_count = 0;
    EXPECT_EQ_INT(LARGE_RESEND_SENT, large_resend(&node, &pub, record->seq_no + 3U, &target));
    EXPECT_EQ_U32(1, sent_count);
    EXPECT_TRUE(node.pacers[0].until_ns > test_mock_now); // counted: a unit's body goes as a second piece
    node.pacers[0].until_ns = test_mock_now + (2U * node.pacers[0].burst_ns);
    sent_count = 0;
    EXPECT_EQ_INT(LARGE_RESEND_SENT, large_resend(&node, &pub, record->seq_no + 3U, &target));
    EXPECT_EQ_U32(0, sent_count);
    EXPECT_EQ_U64(2, node.pacing_repairs_deferred);
    destroy();

    // Unpaced, a backlog means nothing: there is none, and the repair goes.
    opts.net.links[0].pacing_rate = tt_PACING_OFF;
    create(&opts);
    create_publisher(&reliable);
    sample_len = 200;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)data));
    test_mock_send_hook = capture;
    sent_count = 0;
    EXPECT_TRUE(!retransmit_one_sample(&node, &pub, pub.reliable_cache, reliable_cache_depth(pub.reliable_cache),
                                       pub.seq_no, &target));
    EXPECT_EQ_U32(1, sent_count);
    EXPECT_EQ_U64(0, node.pacers[0].until_ns);
    destroy();
}

// ---- Checks: each bound accepted, the value past it refused.

static void expect_created(const struct tt_InitOptions* opts, tt_ret_t expected, const char* what) {
    test_mock_reset();
    test_mock_node_id = 7;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
    test_mock_link_mtu = -1;
    struct tt_Context created;
    memset(&created, 0, sizeof(created));
    tt_ret_t ret = tt_Context_create_with(&created, opts);
    if (ret != expected) {
        printf("test_send_pacing: %s: expected %d, got %d\n", what, (int)expected, (int)ret);
    }
    EXPECT_EQ_INT(expected, ret);
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&created));
    }
}

static void test_validation(void) {
    struct tt_InitOptions opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[0].pacing_rate = tt_LINK_RATE_MIN;
    expect_created(&opts, tt_RET_OK, "pacing_rate tt_LINK_RATE_MIN");
    opts.net.links[0].pacing_rate = tt_LINK_RATE_MIN - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "pacing_rate below tt_LINK_RATE_MIN");

    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].pacing_rate = T1S_RATE;
    expect_created(&opts, tt_RET_OK, "pacing_rate equal to link_rate");
    opts.net.links[0].pacing_rate = T1S_RATE + 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "pacing_rate above link_rate");

    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].pacing_burst = tt_PACING_BURST_MIN;
    expect_created(&opts, tt_RET_OK, "pacing_burst tt_PACING_BURST_MIN");
    opts.net.links[0].pacing_burst = tt_PACING_BURST_MIN - 1U;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "pacing_burst below tt_PACING_BURST_MIN");

    opts = one_link(tt_LINK_PROFILE_ETHERNET);
    opts.net.links[0].pacing_burst = 8000;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "pacing_burst on an unpaced ETHERNET link");
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].pacing_rate = tt_PACING_OFF;
    opts.net.links[0].pacing_burst = 8000;
    expect_created(&opts, tt_RET_INVALID_ARGUMENT, "pacing_burst on a T1S link with pacing off");
    opts.net.links[0].pacing_rate = 0;
    expect_created(&opts, tt_RET_OK, "pacing_burst on a T1S link paced by its profile");

    // A T1S link_rate at tt_LINK_RATE_MIN: the default rate is raised to tt_LINK_RATE_MIN, not refused.
    opts = one_link(tt_LINK_PROFILE_T1S);
    opts.net.links[0].link_rate = tt_LINK_RATE_MIN;
    expect_created(&opts, tt_RET_OK, "T1S at tt_LINK_RATE_MIN");
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_only_t1s_paces_by_default();
    test_t1s_paces_a_large_sample();
    test_a_unit_longer_than_the_burst();
    test_two_links_two_buckets();
    test_each_field_changes_the_send();
    test_ordinary_sends_fill_the_bucket();
    test_retransmissions_are_paced();
    test_validation();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_send_pacing: all tests passed\n");
    return 0;
}
