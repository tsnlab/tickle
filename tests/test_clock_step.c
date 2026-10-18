/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A step of the date in the middle of a RELIABLE stream changes nothing but the timestamps (DESIGN.md section 2,
// "Time"): every deadline is on the monotonic clock (tt_get_ns()), and only the DATA timestamp is on the date
// (tt_get_wall_ns()).
//
// Two contexts in one thread, an in-memory socket per context, and a HAL with two clocks: a monotonic one that moves
// only when the test moves it, and a date that is that plus an offset the test can step by hours, as NTP or an
// operator's `date -s` does. A RELIABLE publisher with a lifespan and a heartbeat streams to a RELIABLE subscriber;
// one sample is lost on the way just before the step, and the date then jumps three hours forward (or back). After
// it, every sample must still be delivered, the lost one by repair; neither side may forget the other; the
// application's own scheduled work must run at its usual rate; and each sample's source timestamp must be the date it
// was published at, once the receiver has read the date again (tt_CONTEXT_UPDATE_INTERVAL).
//
// The control is the core before the split, kept runnable rather than described: built with
// -DTEST_CLOCK_STEP_ONE_CLOCK this HAL's tt_get_ns() returns the date, which is what hal_linux.c's did until
// 2026-10-16 (CLOCK_REALTIME for everything). The file uses no field the split added, so it also builds against the
// core before it, and both cases must fail there - forward, the lifespan expires the lost sample's repair; back,
// every scheduled entry (heartbeat, retry, the application's tick) waits three hours. Every case prints what it did
// beside its verdict: a step that did not happen or a sample that was never lost would pass for the wrong reason.
//
// A publish reads one clock (2026-10-16, publish_clock() in tickle.c): the monotonic one, with the date it stamps
// derived from it by the offset the context reads again every tt_CONTEXT_UPDATE_INTERVAL. So the writer's stamp - on
// the wire and to its own local subscriber - may also be the date before a step until that read, the same grace the
// receiver has. This HAL counts its clock reads, and every publish of the stream, and of a zero-copy publisher that
// fills a datagram, must take exactly one: the core before the change took two (three for a leased writer).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_common.h"

#define NODES 2
#define QUEUE_SLOTS 4096
#define DATAGRAM_MAX 1600
#define TEST_HOST_IP 0x0a000001U
#define STEP_NS (10ULL * tt_MILLISECOND)             // the clock between two rounds of polls
#define DATE_AT_START_NS (1792000000ULL * tt_SECOND) // 2026-10: the 32-bit microseconds wrap in it
#define DATE_STEP_NS (3ULL * 3600ULL * tt_SECOND)    // three hours: far outside the +-35.8 min window
#define LIFESPAN_NS (2ULL * tt_SECOND)               // the writer's: a repair older than this is not sent
#define HEARTBEAT_NS (100ULL * tt_MILLISECOND)       // so a lost tail is asked for too
#define TICK_NS (100ULL * tt_MILLISECOND)            // the application's own periodic work
#define LEASE_NS (3ULL * tt_SECOND)                  // the writer's liveliness lease, asserted by each publish
#define AFTER_STEP_ROUNDS 400                        // 4 s of stream after the step
#define SETTLE_ROUNDS 100                            // and 1 s with no new sample
#define STAMP_GRACE_NS \
    (tt_CONTEXT_UPDATE_INTERVAL + (100ULL * tt_MILLISECOND)) // stamps misplaced until the date is read
#define MAX_SAMPLES 1024

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_CONTEXT_ADDRESS,
    .port = _tt_CONTEXT_PORT,
    .broadcast = _tt_CONTEXT_BROADCAST,
    .context_id = tt_CONTEXT_ID_INVALID,
};

// ---- The HAL: one FIFO of datagrams per context, and two clocks.

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
static uint64_t now_ns;         // monotonic: moves only forward, only when the test moves it
static uint64_t date_offset_ns; // the date is now_ns + this; a step of the date changes only this
static int next_node_id = 1;
static bool drop_next_from_writer;   // the next datagram node 1 sends is lost
static const uint8_t* zerocopy_body; // a body sent from here was sent from the caller's own bytes
static uint32_t zerocopy_sends;
static uint32_t dropped;

static uint64_t clock_reads; // every tt_get_ns() and tt_get_wall_ns()

uint64_t tt_get_ns(void) {
    clock_reads++;
#ifdef TEST_CLOCK_STEP_ONE_CLOCK
    return now_ns + date_offset_ns; // the control: every deadline on the date, as before the split
#else
    return now_ns;
#endif
}

uint64_t tt_get_wall_ns(void) {
    clock_reads++;
    return now_ns + date_offset_ns;
}

uint64_t tt_timer_resolution_ns(void) {
    return 1;
}

int32_t tt_get_node_id(void) {
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

static uint8_t port_to_id(uint16_t port) {
    return (uint8_t)(port - 20000U);
}

static int32_t send_one(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                        uint32_t ip, uint16_t port) {
    if (hdr_len + body_len > DATAGRAM_MAX) {
        return -1;
    }
    if (body != NULL && body == zerocopy_body) {
        zerocopy_sends++;
    }
    if (node->id == 1 && drop_next_from_writer) {
        drop_next_from_writer = false;
        dropped++;
        return (int32_t)(hdr_len + body_len); // "sent", and lost on the way
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

// Nothing queued: the wait ends at once. The clock moves only between rounds (run_round()), so every context in a
// round sees the same time.
int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    (void)timeout;
    struct queue* q = &queues[node->id];
    return q->count > 0 ? pop(q, buf, len, ip, port) : -1;
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

#if tt_SEGMENT_ENABLED
// No segment: every attach is refused, so the two contexts talk over the queues above.
void* tt_segment_create(const char* path, size_t bytes) {
    (void)path;
    (void)bytes;
    return NULL;
}
void* tt_segment_attach(const char* path, size_t bytes, uint8_t* why) {
    (void)path;
    (void)bytes;
    *why = (uint8_t)tt_SEGMENT_ABSENT;
    return NULL;
}
void tt_segment_detach(void* mapping, size_t bytes) {
    (void)mapping;
    (void)bytes;
}
void tt_segment_unlink(const char* path) {
    (void)path;
}
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
#endif

// ---- The stream: one RELIABLE topic, a sequence number per sample, and the date each was published at.

struct sample {
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

static struct tt_Context writer;
static struct tt_Context reader;
static struct tt_TypeSupport topic = {
    .name = "clock_step",
    .data_size = sizeof(struct sample),
    .data_encode_size = sample_encode_size,
    .data_encode = sample_encode,
    .data_decode = sample_decode,
    .data_free = sample_free,
};
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
#if tt_LOCAL_DELIVERY
// A subscriber in the writer's own context (rmw_tickle's build): handed the date the publish read, to the nanosecond.
static struct tt_Subscriber local_sub;
static uint64_t published_date_exact_ns[MAX_SAMPLES];
static uint32_t local_delivered;
static uint32_t local_stamps_wrong;
static uint64_t local_wrong_mono_ns[MAX_SAMPLES]; // when each sample handed a wrong stamp was published
static uint32_t local_wrong_count;
#endif
static uint64_t published_mono_ns[MAX_SAMPLES];
#if tt_LOCAL_DELIVERY

static void on_local_sample(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)seq_no;
    const struct sample* sample = (const struct sample*)data;
    local_delivered++;
    if (sample->seq < MAX_SAMPLES && time != published_date_exact_ns[sample->seq]) {
        local_stamps_wrong++;
        local_wrong_mono_ns[local_wrong_count < MAX_SAMPLES ? local_wrong_count : MAX_SAMPLES - 1] =
            published_mono_ns[sample->seq];
        local_wrong_count++;
    }
}
#endif

static uint32_t published;
static uint32_t delivered;
static uint32_t out_of_order;                   // a delivered seq that is not the one after the last
static uint64_t published_date_ns[MAX_SAMPLES]; // the date at each publish, to the microsecond the wire carries
static uint32_t publish_reads_min = UINT32_MAX; // clock reads one tt_Publisher_publish() took, fewest and most
static uint32_t publish_reads_max;
static uint32_t publishes_counted;
static uint32_t publishes_with_match_heartbeat;
static uint64_t delivered_stamp_ns[MAX_SAMPLES]; // the source timestamp the subscriber was handed
static uint32_t ticks;                           // the application's scheduled work, run
static struct tt_Discovery reader_discovery;     // what the reader knows of remote entities, with their leases
static uint32_t departures;                      // remote entities the reader declared gone

static void on_discovery(struct tt_Context* node, uint8_t context_id, uint32_t endpoint_id, uint8_t kind, bool departed,
                         void* param) {
    (void)node;
    (void)context_id;
    (void)endpoint_id;
    (void)kind;
    (void)param;
    departures += departed ? 1U : 0U;
}

static void on_sample(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)seq_no;
    const struct sample* sample = (const struct sample*)data;
    if (sample->seq != delivered + 1) {
        out_of_order++;
    }
    if (sample->seq < MAX_SAMPLES) {
        delivered_stamp_ns[sample->seq] = time;
    }
    delivered++;
}

static void tick(struct tt_Context* node, uint64_t time, void* param) {
    (void)param;
    ticks++;
    (void)tt_Context_schedule(node, time + TICK_NS, tick, NULL);
}

static void publish_one(void) {
    struct sample s = {++published};
    if (published < MAX_SAMPLES) {
        published_date_ns[published] = (tt_get_wall_ns() / tt_MICROSECOND) * tt_MICROSECOND;
#if tt_LOCAL_DELIVERY
        published_date_exact_ns[published] = tt_get_wall_ns();
#endif
        published_mono_ns[published] = now_ns;
    }
    // A publish that carries the match Heartbeat a new reader is owed (put_match_heartbeat()) also schedules a flush
    // and arms the idle HEARTBEAT, a few times per match: counted apart, as what it is.
    const bool match_heartbeat = pub.match_heartbeat_pending;
    const uint64_t reads_before = clock_reads;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&s));
    const uint32_t reads = (uint32_t)(clock_reads - reads_before);
    if (match_heartbeat) {
        publishes_with_match_heartbeat++;
        return;
    }
    publishes_counted++;
    publish_reads_min = reads < publish_reads_min ? reads : publish_reads_min;
    publish_reads_max = reads > publish_reads_max ? reads : publish_reads_max;
}

static void run_round(bool with_sample) {
    now_ns += STEP_NS;
    if (with_sample) {
        publish_one();
    }
    (void)tt_Context_poll(&writer, 0);
    (void)tt_Context_poll(&reader, 0);
    (void)tt_Context_poll(&writer, 0);
}

static void setup(struct tt_ReliableCache* cache) {
    memset(queues, 0, sizeof(queues));
    memset(published_date_ns, 0, sizeof(published_date_ns));
    memset(published_mono_ns, 0, sizeof(published_mono_ns));
    memset(delivered_stamp_ns, 0, sizeof(delivered_stamp_ns));
    published = 0;
    delivered = 0;
    out_of_order = 0;
    ticks = 0;
    departures = 0;
    publish_reads_min = UINT32_MAX;
    publish_reads_max = 0;
    publishes_counted = 0;
    publishes_with_match_heartbeat = 0;
    memset(&reader_discovery, 0, sizeof(reader_discovery));
    dropped = 0;
    drop_next_from_writer = false;
    now_ns = tt_SECOND;
    date_offset_ns = DATE_AT_START_NS;
    next_node_id = 1;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&writer));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&reader));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_discovery(&reader, &reader_discovery, on_discovery, NULL));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&writer, &pub, &topic, "clock_step"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&reader, &sub, &topic, "clock_step", on_sample));
#if tt_LOCAL_DELIVERY
    local_delivered = 0;
    local_stamps_wrong = 0;
    local_wrong_count = 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&writer, &local_sub, &topic, "clock_step", on_local_sample));
#endif
    pub.reliable = true;
    pub.reliable_cache = cache;
    pub.lifespan_duration_ns = LIFESPAN_NS;
    pub.liveliness_lease_duration_ns = LEASE_NS;
    sub.reliable = true;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_set_heartbeat_period(&pub, HEARTBEAT_NS));
    EXPECT_TRUE(tt_Context_schedule(&writer, tt_get_ns() + TICK_NS, tick, NULL));
    for (int round = 0; round < 200; round++) {
        run_round(false);
    }
}

static void teardown(void) {
    tt_Context_destroy(&writer);
    tt_Context_destroy(&reader);
}

// The case: a stream, one sample lost, the date stepped by `step` (signed) right after it, and the stream on.
static void stream_across_a_step(const char* name, int64_t step) {
    struct tt_ReliableCacheIndex index[64];
    uint8_t arena[tt_RELIABLE_CACHE_ARENA_BYTES(64, tt_RELIABLE_RECORD_BYTES(64))];
    struct tt_ReliableCache cache;
    memset(index, 0, sizeof(index));
    memset(arena, 0, sizeof(arena));
    memset(&cache, 0, sizeof(cache));
    cache.index = index;
    cache.capacity = 64;
    cache.depth = 64;
    cache.arena = arena;
    cache.arena_size = (uint32_t)sizeof(arena);

    setup(&cache);
    const bool matched = count_peers(pub.peers) == 1;
    EXPECT_TRUE(matched); // discovery done: the stream below is unicast to a known reader

    for (int round = 0; round < 100; round++) {
        run_round(true);
    }
    const uint32_t delivered_before = delivered;
    EXPECT_EQ_U32(published, delivered_before);

    // The lost sample, then the step - before the reader has even seen the gap, so the whole repair (the gap seen,
    // the request, the resend from the cache) happens on the far side of it.
    drop_next_from_writer = true;
    publish_one();
    const uint32_t lost_seq = published;
    const uint64_t date_before = tt_get_wall_ns();
    date_offset_ns = (uint64_t)((int64_t)date_offset_ns + step);
    const int64_t stepped = (int64_t)(tt_get_wall_ns() - date_before);
    const uint64_t step_at_mono = now_ns;
    const uint32_t ticks_before = ticks;
    // The reader's liveliness check runs before anything more arrives from the writer, as it does whenever the step
    // lands in a quiet moment: what it judges then is the clock alone.
    now_ns += STEP_NS;
    (void)tt_Context_poll(&reader, 0);

    for (int round = 0; round < AFTER_STEP_ROUNDS; round++) {
        run_round(true);
    }
    for (int round = 0; round < SETTLE_ROUNDS; round++) {
        run_round(false);
    }
    const uint32_t ticks_after = ticks - ticks_before;
    const uint64_t elapsed = now_ns - step_at_mono;

    // Each delivered stamp against the date its sample was published at. A stamp of a sample published before the
    // receiver had read the date again may sit a whole number of 71.6-minute spans away; none after.
    uint32_t stamps_checked = 0;
    uint32_t stamps_wrong = 0;
    uint32_t stamps_in_grace_wrong = 0;
    for (uint32_t seq = 1; seq <= published && seq < MAX_SAMPLES; seq++) {
        if (delivered_stamp_ns[seq] == 0) {
            continue;
        }
        bool right = delivered_stamp_ns[seq] == published_date_ns[seq];
        bool in_grace =
            published_mono_ns[seq] >= step_at_mono && published_mono_ns[seq] < step_at_mono + STAMP_GRACE_NS;
        if (in_grace) {
            stamps_in_grace_wrong += right ? 0U : 1U;
            continue;
        }
        stamps_checked++;
        stamps_wrong += right ? 0U : 1U;
    }

    const bool reader_knows_writer = reader.update_seen[writer.id] && departures == 0;
    const bool writer_knows_reader = count_peers(pub.peers) == 1;
    printf("%s: date stepped %+.1f h at sample %u (lost: %u datagram), %u published, %u delivered (%u out of order); "
           "the application's tick ran %u times in %.1f s; reader knows writer %s (%u entity departures), writer knows "
           "reader %s; %u stamps checked, %u wrong, %u wrong within %.1f s of the step\n",
           name, (double)stepped / 3600e9, lost_seq, dropped, published, delivered, out_of_order, ticks_after,
           (double)elapsed / 1e9, reader_knows_writer ? "yes" : "NO", departures, writer_knows_reader ? "yes" : "NO",
           stamps_checked, stamps_wrong, stamps_in_grace_wrong, (double)(STAMP_GRACE_NS) / 1e9);

    // Treatment: the date really stepped, the sample really was lost, and the stream ran on long enough to be read.
    EXPECT_TRUE(stepped == step);
    EXPECT_EQ_U32(1, dropped);
    EXPECT_TRUE(published > delivered_before + AFTER_STEP_ROUNDS);
    // Nothing forgotten, nothing lost: the repair went, and both ends still know each other.
    EXPECT_EQ_U32(published, delivered);
    EXPECT_EQ_U32(0, out_of_order);
    EXPECT_TRUE(reader_knows_writer);
    EXPECT_TRUE(writer_knows_reader);
    // The application's scheduled work at its own rate across the step: neither stalled nor run in a burst.
    const uint32_t expected_ticks = (uint32_t)(elapsed / TICK_NS);
    EXPECT_TRUE(ticks_after + 1 >= expected_ticks && ticks_after <= expected_ticks + 1);
    // The stamps are the date: before the step, and after it once the receiver has read the date again.
    EXPECT_TRUE(stamps_checked > (uint32_t)(AFTER_STEP_ROUNDS / 2));
    EXPECT_EQ_U32(0, stamps_wrong);
    // One clock read per publish, every publish of the stream above but the few that carried the match Heartbeat.
    printf("%s: %u to %u clock reads per publish (leased, RELIABLE, staged) over %u publishes, %u more carried the "
           "match Heartbeat\n",
           name, publish_reads_min, publish_reads_max, publishes_counted, publishes_with_match_heartbeat);
    EXPECT_TRUE(publishes_counted + publishes_with_match_heartbeat == published);
    EXPECT_TRUE(publishes_with_match_heartbeat * 10 < published); // a few, at the match
    EXPECT_EQ_U32(1, publish_reads_min);
    EXPECT_EQ_U32(1, publish_reads_max);
#if tt_LOCAL_DELIVERY
    // The date of the publish, to the nanosecond - except for a sample published before the writer read the date
    // again after the step, which may carry the date from before it (publish_clock()).
    uint32_t local_wrong_outside_grace = 0;
    for (uint32_t i = 0; i < local_wrong_count && i < MAX_SAMPLES; i++) {
        bool in_grace =
            local_wrong_mono_ns[i] >= step_at_mono && local_wrong_mono_ns[i] < step_at_mono + STAMP_GRACE_NS;
        local_wrong_outside_grace += in_grace ? 0U : 1U;
    }
    printf("%s: %u delivered in the writer's own context, %u with a stamp other than the date of the publish, %u of "
           "them outside %.1f s of the step\n",
           name, local_delivered, local_stamps_wrong, local_wrong_outside_grace, (double)(STAMP_GRACE_NS) / 1e9);
    EXPECT_EQ_U32(published, local_delivered); // the lost datagram is no loss here: local delivery sends nothing
    EXPECT_EQ_U32(local_stamps_wrong, local_wrong_count);
    EXPECT_EQ_U32(0, local_wrong_outside_grace);
#endif
    teardown();
}

static void test_a_date_step_forward_changes_only_the_stamps(void) {
    stream_across_a_step("forward", (int64_t)DATE_STEP_NS);
}

static void test_a_date_step_back_changes_only_the_stamps(void) {
    stream_across_a_step("back", -(int64_t)DATE_STEP_NS);
}

// ---- A zero-copy publish (data_encode_inplace, a sample that fills a datagram) reads one clock too.

#define BIG_BODY                                                                                       \
    ((uint32_t)(tt_MAX_BUFFER_LENGTH - sizeof(struct tt_Header) - sizeof(struct tt_SubmessageHeader) - \
                sizeof(struct tt_DataHeader)) &                                                        \
     ~3U)
static uint8_t big_body[BIG_BODY];
static uint32_t big_delivered;
static uint32_t big_local_delivered;
static uint32_t big_local_stamps_wrong;

static int32_t big_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)BIG_BODY;
}

static int32_t big_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)data;
    if (len < BIG_BODY) {
        return -1;
    }
    memcpy(payload, big_body, BIG_BODY);
    return (int32_t)BIG_BODY;
}

static int32_t big_encode_inplace(struct tt_Data* data, const uint8_t** payload_out) {
    (void)data;
    *payload_out = big_body;
    return (int32_t)BIG_BODY;
}

static int32_t big_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
    (void)data;
    (void)payload;
    (void)native;
    return (int32_t)len;
}

static void on_big(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)seq_no;
    (void)data;
    big_delivered++;
}

#if tt_LOCAL_DELIVERY
static void on_big_local(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)seq_no;
    (void)data;
    big_local_delivered++;
    // The date of the publish: nothing moved either clock since. Read here, so the read is not the publish's.
    const uint64_t reads = clock_reads;
    big_local_stamps_wrong += time == tt_get_wall_ns() ? 0U : 1U;
    clock_reads = reads;
}
#endif

static void test_a_zero_copy_publish_reads_one_clock(void) {
    struct tt_ReliableCacheIndex index[64];
    uint8_t arena[tt_RELIABLE_CACHE_ARENA_BYTES(64, tt_RELIABLE_RECORD_BYTES(64))];
    struct tt_ReliableCache cache;
    memset(index, 0, sizeof(index));
    memset(arena, 0, sizeof(arena));
    memset(&cache, 0, sizeof(cache));
    cache.index = index;
    cache.capacity = 64;
    cache.depth = 64;
    cache.arena = arena;
    cache.arena_size = (uint32_t)sizeof(arena);
    setup(&cache);

    static struct tt_TypeSupport big_topic = {
        .name = "clock_step_big",
        .data_size = sizeof(struct sample),
        .data_encode_size = big_encode_size,
        .data_encode = big_encode,
        .data_encode_inplace = big_encode_inplace,
        .data_decode = big_decode,
        .data_free = sample_free,
    };
    static struct tt_Publisher big_pub;
    static struct tt_Subscriber big_sub;
    big_delivered = 0;
    big_local_delivered = 0;
    big_local_stamps_wrong = 0;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&writer, &big_pub, &big_topic, "clock_step_big"));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&reader, &big_sub, &big_topic, "clock_step_big", on_big));
#if tt_LOCAL_DELIVERY
    static struct tt_Subscriber big_local_sub;
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Context_create_subscriber(&writer, &big_local_sub, &big_topic, "clock_step_big", on_big_local));
#endif
    big_pub.liveliness_lease_duration_ns = LEASE_NS;
    for (int round = 0; round < 200; round++) {
        run_round(false);
    }
    EXPECT_TRUE(count_peers(big_pub.peers) == 1);

    zerocopy_body = big_body;
    zerocopy_sends = 0;
    uint32_t reads_min = UINT32_MAX;
    uint32_t reads_max = 0;
    const uint32_t rounds = 50;
    for (uint32_t i = 0; i < rounds; i++) {
        now_ns += STEP_NS;
        struct sample s = {i};
        const uint64_t reads_before = clock_reads;
        EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&big_pub, (struct tt_Data*)&s));
        const uint32_t reads = (uint32_t)(clock_reads - reads_before);
        reads_min = reads < reads_min ? reads : reads_min;
        reads_max = reads > reads_max ? reads : reads_max;
        (void)tt_Context_poll(&writer, 0);
        (void)tt_Context_poll(&reader, 0);
    }
    zerocopy_body = NULL;
    printf("zero-copy: %u of %u publishes sent from the caller's bytes, %u delivered, %u in the writer's own context "
           "(%u stamped other than the date); %u to %u clock reads per publish (leased)\n",
           zerocopy_sends, rounds, big_delivered, big_local_delivered, big_local_stamps_wrong, reads_min, reads_max);
    EXPECT_EQ_U32(rounds, zerocopy_sends); // the treatment: every one took the zero-copy path
    EXPECT_EQ_U32(rounds, big_delivered);
#if tt_LOCAL_DELIVERY
    EXPECT_EQ_U32(rounds, big_local_delivered);
    EXPECT_EQ_U32(0, big_local_stamps_wrong);
#endif
    EXPECT_EQ_U32(1, reads_min);
    EXPECT_EQ_U32(1, reads_max);
    teardown();
}

int main(void) {
    test_a_date_step_forward_changes_only_the_stamps();
    test_a_date_step_back_changes_only_the_stamps();
    test_a_zero_copy_publish_reads_one_clock();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_clock_step: all tests passed\n");
    return 0;
}
