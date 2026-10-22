/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A writer's switch from UDP to its peer's segment loses no BEST_EFFORT sample (DESIGN.md section 10, "The switch
// from UDP").
//
// Found 2026-10-11 as test_thread_safety's "unaccounted" loss: node A reaches B over UDP while B's segment is absent
// or still being built (attach ABSENT or BAD_HEADER, asked again only once the summary tick ages the answer),
// then attaches and writes to the ring. B drains its ring before its socket, so it delivered the first ring record
// and then discarded every UDP datagram still queued behind it as older - BEST_EFFORT ordering, counted in
// tt_Subscriber.out_of_order_discarded. 1-224 samples a run there, and only when the thread timing put the switch
// inside the stream, which is why it was a one-in-thirty flake.
//
// Here the timing is staged instead of hoped for: two contexts in one thread, an in-memory socket queue per context,
// a mock clock, and an attach this HAL refuses until the test lets it through. Every case says what it did (how many
// samples took each path) beside its verdict, because a case whose writer never left UDP, or never used it, would pass
// for the wrong reason.
//
// The fence-specific assertions sit behind #ifdef tt_SEGMENT_SPAN_FENCE so that this file also builds against the
// core as it was before the fence, where the first case must fail - its control, kept runnable rather than described.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_common.h"

#if tt_SEGMENT_ENABLED

#define NODES 3           // context ids 1..NODES
#define QUEUE_SLOTS 16384 // never full here: a lost datagram is staged on purpose, not by capacity
#define DATAGRAM_MAX 1600
#define TEST_HOST_IP 0x0a000001U // one host: the only arrangement in which a segment can be shared
#define UDP_SAMPLES 50           // published while the attach is refused: queued in B's socket
#define RING_SAMPLES 50          // published after the switch
#define STEP_NS 20000000ULL      // discovery: the clock between two polls of each context

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_CONTEXT_ADDRESS,
    .port = _tt_CONTEXT_PORT,
    .broadcast = _tt_CONTEXT_BROADCAST,
    .context_id = tt_CONTEXT_ID_INVALID,
};

// ---- The HAL: one FIFO of datagrams per context, segments in heap memory, a clock that moves only when told to.

struct datagram {
    uint32_t len;
    uint8_t from_id;
    uint8_t bytes[DATAGRAM_MAX];
};

struct queue {
    struct datagram slots[QUEUE_SLOTS];
    uint32_t head;
    uint32_t count;
};

static struct queue queues[NODES + 1];
static uint64_t now_ns = 1;
static int next_node_id = 1;
static bool attach_refused;    // tt_segment_attach() answers ABSENT, as for a segment not built yet
static bool drop_fences;       // a fence datagram is lost on the way, as to a full socket buffer
static uint32_t receive_waits; // tt_receive() calls that found nothing, and so "waited"
static int64_t last_wait;      // the timeout the last of them asked for (0: indefinitely)
static uint32_t indefinite_waits;

uint64_t tt_get_ns(void) {
    return now_ns;
}

uint64_t tt_get_wall_ns(void) {
    return now_ns;
}

uint64_t tt_timer_resolution_ns(void) {
    return 1;
}

int32_t tt_get_node_id(const struct tt_Context* node) {
    (void)node;
    return next_node_id++;
}

bool tt_resolve_link(const char* broadcast, uint32_t* addr, uint32_t* netmask, uint32_t* bcast) {
    (void)broadcast;
    *addr = TEST_HOST_IP;
    *netmask = 0xffffff00;
    *bcast = 0x0a0000ff;
    return true;
}

int32_t tt_link_mtu(uint32_t addr) {
    (void)addr;
    return -1;
}

tt_ret_t tt_bind(struct tt_Context* node) {
    (void)node;
    return tt_RET_OK;
}

void tt_close(struct tt_Context* node) {
    (void)node;
}

static void push(uint8_t to_id, uint8_t from_id, const void* hdr, size_t hdr_len, const void* body, size_t body_len) {
    if (to_id == 0 || to_id > NODES) {
        return;
    }
#ifdef tt_SEGMENT_SPAN_FENCE
    if (drop_fences && hdr_len + body_len == tt_SEGMENT_FENCE_LENGTH &&
        ((const uint8_t*)hdr)[0] == (uint8_t)tt_SEGMENT_FENCE_MARKER) {
        return;
    }
#endif
    struct queue* q = &queues[to_id];
    if (q->count == QUEUE_SLOTS) {
        fprintf(stderr, "test harness: queue of context %u full\n", (unsigned)to_id);
        test_failures++;
        return;
    }
    struct datagram* d = &q->slots[(q->head + q->count) % QUEUE_SLOTS];
    d->len = (uint32_t)(hdr_len + body_len);
    d->from_id = from_id;
    memcpy(d->bytes, hdr, hdr_len);
    if (body_len > 0) {
        memcpy(d->bytes + hdr_len, body, body_len);
    }
    q->count++;
}

// A broadcast reaches every other context; a unicast reaches the one its port names (tt_own_address() below).
static uint8_t port_to_id(uint16_t port) {
    return (uint8_t)(port - 20000U);
}

static int32_t send_one(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                        uint32_t ip, uint16_t port) {
    if (hdr_len + body_len > DATAGRAM_MAX) {
        return -1;
    }
    if (ip == 0) {
        for (uint8_t id = 1; id <= NODES; id++) {
            if (id != node->id) {
                push(id, node->id, hdr, hdr_len, body, body_len);
            }
        }
    } else {
        push(port_to_id(port), node->id, hdr, hdr_len, body, body_len);
    }
    return (int32_t)(hdr_len + body_len);
}

int32_t tt_send(struct tt_Context* node, const void* buf, size_t len) {
    return send_one(node, buf, len, NULL, 0, 0, 0);
}

int32_t tt_send_to(struct tt_Context* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    return send_one(node, buf, len, NULL, 0, ip, port);
}

int32_t tt_send_iov(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
    return send_one(node, hdr, hdr_len, body, body_len, ip, port);
}

int32_t tt_send_batch(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (send_one(node, datagrams[i].head, datagrams[i].head_len, datagrams[i].body, datagrams[i].body_len,
                     datagrams[i].ip, datagrams[i].port) < 0) {
            return -1;
        }
    }
    return (int32_t)count;
}

static int32_t pop(struct queue* q, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct datagram* d = &q->slots[q->head];
    uint32_t n = d->len < len ? d->len : (uint32_t)len;
    memcpy(buf, d->bytes, n);
    *ip = TEST_HOST_IP;
    *port = (uint16_t)(20000U + d->from_id);
    q->head = (q->head + 1) % QUEUE_SLOTS;
    q->count--;
    return (int32_t)n;
}

// Nothing queued: the wait "elapses" on the mock clock, and is recorded, so a case can see what a poll waited for.
int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    struct queue* q = &queues[node->id];
    if (q->count > 0) {
        return pop(q, buf, len, ip, port);
    }
    receive_waits++;
    last_wait = timeout;
    if (timeout > 0) {
        now_ns += (uint64_t)timeout;
    } else {
        indefinite_waits++; // a real HAL would block here for good; this one returns so the test can say so
    }
    return -1;
}

int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct queue* q = &queues[node->id];
    return q->count > 0 ? pop(q, buf, len, ip, port) : -1;
}

uint32_t tt_rx_buffered(const struct tt_Context* node) {
    return queues[node->id].count;
}

bool tt_rx_maybe_ready(struct tt_Context* node) {
    (void)node;
    return true;
}

tt_ret_t tt_wake_signal(struct tt_Context* node) {
    (void)node;
    return tt_RET_OK;
}

uint8_t tt_claim_context_id(struct tt_Context* node, uint8_t preferred, const uint8_t* avoid, uint32_t salt) {
    (void)node;
    (void)avoid;
    (void)salt;
    return preferred;
}

bool tt_is_own_address(const struct tt_Context* node, uint32_t ip, uint16_t port) {
    (void)node;
    (void)ip;
    (void)port;
    return false;
}

void tt_own_address(const struct tt_Context* node, uint32_t* ip, uint16_t* port) {
    if (ip != NULL) {
        *ip = TEST_HOST_IP;
    }
    if (port != NULL) {
        *port = (uint16_t)(20000U + node->id);
    }
}

uint64_t tt_thread_cpu_ns(void) {
    return 0;
}

#define MAX_SEGMENTS 8
static struct {
    char path[tt_SEGMENT_PATH_LENGTH];
    void* region;
    size_t bytes;
    bool present;
} segments[MAX_SEGMENTS];

void* tt_segment_create(const char* path, size_t bytes) {
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        if (segments[i].region != NULL && strcmp(segments[i].path, path) == 0) {
            segments[i].present = false; // a real create unlinks first
        }
    }
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        if (segments[i].region == NULL) {
            segments[i].region = calloc(1, bytes);
            snprintf(segments[i].path, sizeof(segments[i].path), "%s", path);
            segments[i].bytes = bytes;
            segments[i].present = segments[i].region != NULL;
            return segments[i].region;
        }
    }
    return NULL;
}

void* tt_segment_attach(const char* path, size_t bytes, uint8_t* why) {
    *why = (uint8_t)tt_SEGMENT_ABSENT;
    if (attach_refused) {
        return NULL;
    }
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        if (segments[i].region != NULL && segments[i].present && strcmp(segments[i].path, path) == 0) {
            if (segments[i].bytes < bytes) {
                *why = (uint8_t)tt_SEGMENT_BAD_HEADER;
                return NULL;
            }
            *why = (uint8_t)tt_SEGMENT_ATTACHED;
            return segments[i].region;
        }
    }
    return NULL;
}

void tt_segment_detach(void* mapping, size_t bytes) {
    (void)mapping;
    (void)bytes; // the region outlives its attachers, as a real mapping's file does
}

void tt_segment_unlink(const char* path) {
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        if (segments[i].region != NULL && strcmp(segments[i].path, path) == 0) {
            segments[i].present = false;
        }
    }
}

// No FIFO doorbell: peers ring over (this) UDP, as on a platform without one.
int32_t tt_segment_bell_create(struct tt_Context* node, const char* path) {
    (void)node;
    (void)path;
    return -1;
}
void tt_segment_bell_destroy(struct tt_Context* node, const char* path) {
    (void)node;
    (void)path;
}
int32_t tt_segment_bell_open(const char* path) {
    (void)path;
    return -1;
}
void tt_segment_bell_ring(int32_t bell) {
    (void)bell;
}
void tt_segment_bell_close(int32_t bell) {
    (void)bell;
}

// ---- The stream: one BEST_EFFORT topic per writer, a sequence number per sample.

struct sample {
    uint32_t writer;
    uint32_t seq;
};

static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(struct sample);
}

static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(struct sample)) {
        return -1;
    }
    memcpy(payload, data, sizeof(struct sample));
    return (int32_t)sizeof(struct sample);
}

static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(struct sample)) {
        return -1;
    }
    memcpy(data, payload, sizeof(struct sample));
    return (int32_t)sizeof(struct sample);
}

static void sample_free(struct tt_Data* data) {
    (void)data;
}

static struct tt_Context node_a; // writer
static struct tt_Context node_b; // reader: owns the segment
static struct tt_Context node_c; // a second writer, in the case that has two
static struct tt_TypeSupport topic = {
    .name = "switch",
    .data_size = sizeof(struct sample),
    .data_encode_size = sample_encode_size,
    .data_encode = sample_encode,
    .data_decode = sample_decode,
    .data_free = sample_free,
};
static struct tt_Publisher pub_a;
static struct tt_Publisher pub_c;
static struct tt_Subscriber sub_b;

#define WRITERS 2 // index 0: node A, 1: node C
static uint32_t received[WRITERS];
static uint32_t delivered[WRITERS];
static uint32_t backwards[WRITERS];
static uint32_t published[WRITERS];

static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    const struct sample* s = (const struct sample*)data;
    if (s->writer >= WRITERS) {
        return;
    }
    if (s->seq <= received[s->writer]) {
        backwards[s->writer]++;
    }
    received[s->writer] = s->seq;
    delivered[s->writer]++;
}

static void publish(struct tt_Publisher* pub, uint32_t writer) {
    struct sample s = {writer, ++published[writer]};
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(pub, (struct tt_Data*)&s));
}

static void poll_all(bool with_c) {
    (void)tt_Context_poll(&node_a, 0);
    (void)tt_Context_poll(&node_b, 0);
    if (with_c) {
        (void)tt_Context_poll(&node_c, 0);
    }
}

static void free_segments(void) {
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        free(segments[i].region);
        memset(&segments[i], 0, sizeof(segments[i]));
    }
}

// Two (or three) contexts that have discovered each other, with every attach refused until the case allows it.
static void setup(bool with_c) {
    memset(queues, 0, sizeof(queues));
    memset(received, 0, sizeof(received));
    memset(delivered, 0, sizeof(delivered));
    memset(backwards, 0, sizeof(backwards));
    memset(published, 0, sizeof(published));
    free_segments();
    now_ns = 1;
    next_node_id = 1;
    attach_refused = true;
    drop_fences = false;
    receive_waits = 0;
    indefinite_waits = 0;
    last_wait = 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&node_a));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&node_b));
    if (with_c) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&node_c));
        EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node_c), &pub_c, &topic, "switch",
                                                          &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    }
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node_a), &pub_a, &topic, "switch",
                                                      &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(
                                 tt_Context_default_node(&node_b), &sub_b, &topic, "switch", &TEST_QOS_UNRELIABLE,
                                 &(struct tt_SubscriberOptions) {.callback = on_sample}, NULL, 0));
    for (int step = 0; step < 200; step++) {
        now_ns += STEP_NS;
        poll_all(with_c);
    }
}

static void teardown(bool with_c) {
    tt_Context_destroy(&node_a);
    tt_Context_destroy(&node_b);
    if (with_c) {
        tt_Context_destroy(&node_c);
    }
    free_segments();
}

// The cached segment answers aged past their deadline, as two summary ticks a retry interval apart do it (the stamp,
// then the deadline; segment_recheck_sweep()), without moving the clock the rest of the test reads.
static void age_segment_answers(struct tt_Context* node) {
    segment_recheck_sweep(node, now_ns);
    segment_recheck_sweep(node, now_ns + tt_SEGMENT_ATTACH_RETRY_NS);
}

// Publishes over UDP, with the attach refused, then lets the attach through and publishes until the writer has put
// RING_SAMPLES in the ring. Returns how many samples went over UDP. The reader does not poll meanwhile, so all of
// them are still in its socket when the ring records arrive - the order the defect needs.
static uint32_t switch_writer(struct tt_Context* node, struct tt_Publisher* pub, uint32_t writer) {
    const uint64_t udp_before = node->segment_unattached_to_udp;
    for (int i = 0; i < UDP_SAMPLES; i++) {
        publish(pub, writer);
    }
    // Every one went UNICAST over UDP: a broadcast would mean discovery had not finished, and the case would not be
    // the switch it is about.
    EXPECT_EQ_U64(UDP_SAMPLES, node->segment_unattached_to_udp - udp_before);
    attach_refused = false;
    uint32_t guard = 0;
    while (node->tx_datagrams_by_transport[tt_TRANSPORT_SHM] == 0 && guard++ < 4) {
        age_segment_answers(node); // the absent answer expires, and the attach is asked again
        publish(pub, writer);
    }
    const uint32_t over_udp = (uint32_t)(node->segment_unattached_to_udp - udp_before);
    for (int i = 1; i < RING_SAMPLES; i++) {
        publish(pub, writer);
    }
    EXPECT_EQ_U64(RING_SAMPLES, node->tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    return over_udp;
}

// Polls the reader until its socket and ring are both empty.
static void drain_reader(void) {
    for (int i = 0; i < 1000 && (queues[node_b.id].count > 0 || segment_has_records(&node_b)); i++) {
        now_ns += 10000;
        (void)tt_Context_poll(&node_b, 0);
    }
    EXPECT_EQ_U32(0, queues[node_b.id].count);
    EXPECT_TRUE(!segment_has_records(&node_b));
}

static void report(const char* name, uint32_t over_udp) {
    printf("%s: %u published (%u over UDP, %u over the ring), %u delivered, %u discarded as out of order, "
           "%u backwards\n",
           name, published[0], over_udp, (unsigned)node_a.tx_datagrams_by_transport[tt_TRANSPORT_SHM], delivered[0],
           sub_b.out_of_order_discarded, backwards[0]);
}

// THE DEFECT. Everything the writer sent over UDP is still in the reader's socket when its first ring record arrives;
// every sample must be delivered, in order. Before the fence the reader took the ring first and discarded all
// over_udp of them (and this is the case that fails against that core).
static void test_the_switch_from_udp_loses_nothing(void) {
    setup(false);
    const uint32_t over_udp = switch_writer(&node_a, &pub_a, 0);
    EXPECT_TRUE(over_udp >= UDP_SAMPLES); // treatment: the reader's socket held this many when the ring began
    drain_reader();
    report("switch", over_udp);
    EXPECT_EQ_U32(published[0], delivered[0]);
    EXPECT_EQ_U32(0, sub_b.out_of_order_discarded);
    EXPECT_EQ_U32(0, backwards[0]);
#ifdef tt_SEGMENT_SPAN_FENCE
    EXPECT_EQ_U64(1, node_a.segment_fences_sent);
    EXPECT_EQ_U64(1, node_b.segment_fences_passed);  // passed on its datagram ...
    EXPECT_EQ_U64(0, node_b.segment_fences_expired); // ... not on the deadline, which would also have been lossless
#endif
    teardown(false);
}

// THE CONTROL: the same stream with the reader's socket drained before the writer switches. Nothing is queued
// behind the switch, so this was lossless before the fence too; a harness that lost samples here would be measuring
// itself.
static void test_control_a_reader_that_drained_its_socket_first(void) {
    setup(false);
    for (int i = 0; i < UDP_SAMPLES; i++) {
        publish(&pub_a, 0);
    }
    drain_reader();
    attach_refused = false;
    const uint64_t udp_before = node_a.segment_unattached_to_udp;
    uint32_t guard = 0;
    while (node_a.tx_datagrams_by_transport[tt_TRANSPORT_SHM] == 0 && guard++ < 4) {
        age_segment_answers(&node_a);
        publish(&pub_a, 0);
        drain_reader();
    }
    for (int i = 1; i < RING_SAMPLES; i++) {
        publish(&pub_a, 0);
    }
    drain_reader();
    report("control", UDP_SAMPLES + (uint32_t)(node_a.segment_unattached_to_udp - udp_before));
    EXPECT_TRUE(node_a.tx_datagrams_by_transport[tt_TRANSPORT_SHM] > 0);
    EXPECT_EQ_U32(published[0], delivered[0]);
    EXPECT_EQ_U32(0, sub_b.out_of_order_discarded);
    teardown(false);
}

#ifdef tt_SEGMENT_SPAN_FENCE
// The fence's datagram is lost. The reader holds the ring - reading its socket meanwhile, so the UDP samples still
// arrive first - and gives the fence up at tt_SEGMENT_FENCE_HOLD_NS, not before and not never; a sleep taken while
// it is held is bounded by that deadline rather than indefinite.
static void test_a_lost_fence_holds_the_ring_until_its_deadline(void) {
    setup(false);
    drop_fences = true;
    const uint32_t over_udp = switch_writer(&node_a, &pub_a, 0);
    EXPECT_EQ_U64(1, node_a.segment_fences_sent); // sent, and lost by the "network"
    const uint64_t start = now_ns;
    (void)tt_Context_poll(&node_b, 0); // the fence is met at the head: held
    EXPECT_TRUE(node_b.segment_fence_deadline_ns != 0);
    for (int i = 0; i < 100 && queues[node_b.id].count > 0; i++) {
        (void)tt_Context_poll(&node_b, 0); // the socket, while the ring waits
    }
    EXPECT_EQ_U32(over_udp, delivered[0]); // every UDP sample, and no ring sample yet
    EXPECT_TRUE(now_ns - start < tt_SEGMENT_FENCE_HOLD_NS);
    EXPECT_EQ_U64(0, node_b.segment_fences_expired);
    // Asleep with the fence held: the wait ends by its deadline, not indefinitely.
    receive_waits = 0;
    indefinite_waits = 0;
    (void)tt_Context_poll(&node_b, -1);
    EXPECT_TRUE(receive_waits > 0);
    EXPECT_EQ_U32(0, indefinite_waits);
    EXPECT_TRUE(last_wait > 0 && (uint64_t)last_wait <= tt_SEGMENT_FENCE_HOLD_NS);
    drain_reader();
    report("lost fence", over_udp);
    EXPECT_EQ_U64(1, node_b.segment_fences_expired);
    EXPECT_EQ_U64(0, node_b.segment_fences_passed);
    EXPECT_TRUE(now_ns - start >= tt_SEGMENT_FENCE_HOLD_NS);
    EXPECT_EQ_U32(published[0], delivered[0]);
    EXPECT_EQ_U32(0, sub_b.out_of_order_discarded);
    teardown(false);
}

// The fence's datagram read before its record is met: remembered, and the record then passes at once - no hold.
static void test_a_fence_whose_datagram_came_first_passes_at_once(void) {
    setup(false);
    const uint32_t over_udp = switch_writer(&node_a, &pub_a, 0);
    // The socket alone, ahead of the ring - which a poll never does, so it is done here by hand.
    uint32_t ip = 0;
    uint16_t port = 0;
    int32_t len = 0;
    while ((len = tt_try_receive(&node_b, RX_LANDING(&node_b), tt_MAX_BUFFER_LENGTH, &ip, &port)) >= 0) {
        (void)process_datagram(&node_b, len, ip, port, tt_TRANSPORT_UDP);
    }
    EXPECT_EQ_U32(over_udp, delivered[0]);
    (void)tt_Context_poll(&node_b, 0);
    EXPECT_EQ_U64(0, node_b.segment_fence_deadline_ns); // never held
    drain_reader();
    report("datagram first", over_udp);
    EXPECT_EQ_U64(1, node_b.segment_fences_passed);
    EXPECT_EQ_U64(0, node_b.segment_fences_expired);
    EXPECT_EQ_U32(published[0], delivered[0]);
    EXPECT_EQ_U32(0, sub_b.out_of_order_discarded);
    teardown(false);
}

// Revalidation (tt_SEGMENT_REVALIDATE_NS after the summary tick stamped the answer) re-attaches the same mapping with
// nothing sent over UDP in
// between: no fence, so the steady state never holds its ring.
static void test_a_revalidation_sends_no_fence(void) {
    setup(false);
    (void)switch_writer(&node_a, &pub_a, 0);
    drain_reader();
    const uint32_t attaches = node_a.segment_attach[tt_SEGMENT_ATTACHED];
    for (uint32_t i = 0; i < 3 * 64; i++) {
        if (i % 64 == 0) {
            drain_reader();
            segment_recheck_sweep(&node_a, now_ns); // the attached answer stamped, and due one interval later
            segment_recheck_sweep(&node_a, now_ns + tt_SEGMENT_REVALIDATE_NS);
        }
        publish(&pub_a, 0);
    }
    drain_reader();
    printf("revalidation: %u attaches, %lu fences sent\n", node_a.segment_attach[tt_SEGMENT_ATTACHED] - attaches,
           (unsigned long)node_a.segment_fences_sent);
    EXPECT_TRUE(node_a.segment_attach[tt_SEGMENT_ATTACHED] - attaches >= 2); // treatment: it did revalidate
    EXPECT_EQ_U64(1, node_a.segment_fences_sent);
    EXPECT_EQ_U64(1, node_b.segment_fences_passed + node_b.segment_fences_expired);
    EXPECT_EQ_U32(published[0], delivered[0]);
    teardown(false);
}

// Two writers switching into one reader's ring at once: each fence waits for its own datagram, the second's
// remembered while the first's holds the head.
static void test_two_writers_switching_at_once_lose_nothing(void) {
    setup(true);
    const uint32_t udp_a = switch_writer(&node_a, &pub_a, 0);
    attach_refused = true;
    const uint64_t shm_c = node_c.tx_datagrams_by_transport[tt_TRANSPORT_SHM];
    const uint32_t udp_c = switch_writer(&node_c, &pub_c, 1);
    EXPECT_EQ_U64(0, shm_c);
    drain_reader();
    printf("two writers: A %u/%u, C %u/%u delivered (%u and %u over UDP), %u discarded\n", delivered[0], published[0],
           delivered[1], published[1], udp_a, udp_c, sub_b.out_of_order_discarded);
    EXPECT_EQ_U32(published[0], delivered[0]);
    EXPECT_EQ_U32(published[1], delivered[1]);
    EXPECT_EQ_U32(0, sub_b.out_of_order_discarded);
    EXPECT_EQ_U64(2, node_b.segment_fences_passed);
    EXPECT_EQ_U64(0, node_b.segment_fences_expired);
    teardown(true);
}

// A fence's datagram that arrives after its fence was given up on must not pass the writer's NEXT fence: the
// generation tells them apart. Staged as a writer that switched with its datagram lost, fell back to UDP (as after a
// dead-reader give-up), and switched again with that datagram lost too, while the first one turned up late.
static void test_a_late_fence_does_not_pass_a_newer_one(void) {
    setup(false);
    drop_fences = true;
    const uint32_t first = switch_writer(&node_a, &pub_a, 0);
    for (int i = 0; i < 10 && node_b.segment_fences_expired == 0; i++) {
        now_ns += tt_SEGMENT_FENCE_HOLD_NS;
        drain_reader();
    }
    EXPECT_EQ_U64(1, node_b.segment_fences_expired);
    const uint8_t late[tt_SEGMENT_FENCE_LENGTH] = {(uint8_t)tt_SEGMENT_FENCE_MARKER, node_a.id,
                                                   node_a.segment_fence_generation};
    drop_fences = false;
    push(node_b.id, node_a.id, late, sizeof(late), NULL, 0);
    drop_fences = true;
    drain_reader(); // read now, and remembered
    struct tt_SegmentPeer* entry = &node_a.segment_peers[node_b.id];
    remember_absent(entry, entry->ip, entry->port);
    attach_refused = true;
    const uint64_t shm_before = node_a.tx_datagrams_by_transport[tt_TRANSPORT_SHM];
    for (int i = 0; i < UDP_SAMPLES; i++) {
        publish(&pub_a, 0);
    }
    attach_refused = false;
    entry->recheck_due = true;
    for (int i = 0; i < RING_SAMPLES; i++) {
        publish(&pub_a, 0);
    }
    EXPECT_EQ_U64(shm_before + RING_SAMPLES, node_a.tx_datagrams_by_transport[tt_TRANSPORT_SHM]);
    EXPECT_EQ_U64(2, node_a.segment_fences_sent);
    for (int i = 0; i < 10 && node_b.segment_fences_expired < 2; i++) {
        drain_reader();
        now_ns += tt_SEGMENT_FENCE_HOLD_NS;
    }
    drain_reader();
    printf("late fence: %u published (%u, then %u over UDP), %u delivered, %u discarded, %lu passed, %lu expired\n",
           published[0], first, UDP_SAMPLES, delivered[0], sub_b.out_of_order_discarded,
           (unsigned long)node_b.segment_fences_passed, (unsigned long)node_b.segment_fences_expired);
    EXPECT_EQ_U64(0, node_b.segment_fences_passed);
    EXPECT_EQ_U64(2, node_b.segment_fences_expired);
    EXPECT_EQ_U32(published[0], delivered[0]);
    EXPECT_EQ_U32(0, sub_b.out_of_order_discarded);
    teardown(false);
}

// A ring with no room for the fence refuses the attach: the writer stays on UDP rather than switching unfenced.
static void test_a_full_ring_refuses_the_switch(void) {
    setup(false);
    attach_refused = false;
    publish(&pub_a, 0); // attaches, fenced
    struct tt_SegmentHeader* ring = node_b.own_segment;
    EXPECT_TRUE(ring != NULL);
    uint32_t claimed = 0;
    struct tt_SegmentSlot* slot = NULL;
    while ((slot = segment_claim(ring, &claimed)) != NULL) {
        segment_publish(slot, claimed, 0, TEST_HOST_IP, 20009, 1); // filler, never drained
    }
    struct tt_SegmentPeer* entry = &node_a.segment_peers[node_b.id];
    remember_absent(entry, entry->ip, entry->port); // as after a dead-reader give-up
    entry->recheck_due = true;
    const uint64_t fences = node_a.segment_fences_sent;
    EXPECT_TRUE(peer_segment(&node_a, node_b.id, entry->ip, entry->port) == NULL);
    EXPECT_EQ_U64(fences + 1, node_a.segment_fences_sent); // the datagram went; the record found no room
    EXPECT_TRUE(entry->missing);
    teardown(false);
}
#endif

int main(void) {
    test_the_switch_from_udp_loses_nothing();
    test_control_a_reader_that_drained_its_socket_first();
#ifdef tt_SEGMENT_SPAN_FENCE
    test_a_lost_fence_holds_the_ring_until_its_deadline();
    test_a_fence_whose_datagram_came_first_passes_at_once();
    test_a_revalidation_sends_no_fence();
    test_two_writers_switching_at_once_lose_nothing();
    test_a_late_fence_does_not_pass_a_newer_one();
    test_a_full_ring_refuses_the_switch();
#endif
    if (test_result() != 0) {
        return 1;
    }
    printf("test_segment_switch: all tests passed\n");
    return 0;
}

#else
int main(void) {
    printf("test_segment_switch: skipped - built with tt_SEGMENT_ENABLED=0\n");
    return 0;
}
#endif
