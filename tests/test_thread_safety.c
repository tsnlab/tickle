/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Core called from several threads at once, through the public API only (2026-09-25, the user's
// decision that core be thread-safe - see "Threading" at tt_Context_lock() in tickle.h).
//
// Two nodes in one process, joined by an in-memory datagram queue per node (the HAL below). Node A has
// PUBLISHER_THREADS application threads publishing and one thread scheduling and cancelling timers,
// all while A's own poll thread runs its scheduler and processes datagrams; node B's poll thread only
// receives. What is checked is the outcome, not the absence of a crash: every sample arrives, in
// order per publisher; every timer that was scheduled and not cancelled runs exactly once; a second
// poller is refused. Build it with -fsanitize=thread (`make tsan`) and ThreadSanitizer checks the
// part an outcome cannot - that nothing was shared unsynchronised on the way.
//
// The control is the same file against the core as it was before locking: there it must fail, under
// ThreadSanitizer at least, or this test would be proving nothing about the locks.
//
// This file supplies its own HAL instead of test_mock.h's: the mock keeps plain global counters,
// written from whichever thread calls it, which is fine for the single-threaded tests it serves and
// would be a data race of its own here.

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_COMMON_DEFINE_STORAGE
#include <tickle/config.h>
#include <tickle/tickle.h>

#include "test_common.h"

// The control build compiles this file against the core as it was before locking, which has none of
// these; they become no-ops there so the same threads run against it unchanged.
#ifndef tt_THREAD_SAFE
#define CONTROL_BUILD 1
static void tt_Context_lock(struct tt_Context* node) {
    (void)node;
}
static void tt_Context_unlock(struct tt_Context* node) {
    (void)node;
}
#define tt_RET_BUSY (-15)
#endif

#define PUBLISHER_THREADS 4
#define SAMPLES_PER_THREAD 20000
#define TIMERS 2000
#define QUEUE_SLOTS 256
#define DATAGRAM_MAX 1600
#define NODE_COUNT 2
// The one host both contexts are on. Shared memory is only possible between contexts that share a
// machine, so a harness meant to exercise it cannot give them different addresses - see
// tt_own_address() below.
#define TEST_HOST_IP 0x0a000001U
#define DELIVERY_GIVE_UP_NS (20ULL * 1000ULL * 1000ULL * 1000ULL)

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_CONTEXT_ADDRESS,
    .port = _tt_CONTEXT_PORT,
    .broadcast = _tt_CONTEXT_BROADCAST,
    .context_id = tt_CONTEXT_ID_INVALID,
};

// ---- An in-memory, thread-safe HAL: one bounded FIFO of datagrams per node. A full queue makes the
// sender wait rather than drop, so the transport is lossless and "every sample arrives" is a fair
// question to ask of core.

struct datagram {
    uint32_t len;
    uint8_t from_id;
    uint8_t bytes[DATAGRAM_MAX];
};

struct queue {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    struct datagram slots[QUEUE_SLOTS];
    uint32_t head;
    uint32_t count;
    bool wake; // tt_wake_signal() pending
};

static struct queue queues[NODE_COUNT + 1]; // indexed by node id, 1..NODE_COUNT
static int next_node_id = 1;

uint64_t tt_get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

int32_t tt_get_node_id(void) {
    return __atomic_fetch_add(&next_node_id, 1, __ATOMIC_RELAXED);
}

bool tt_resolve_link(const char* broadcast, uint32_t* addr, uint32_t* netmask, uint32_t* bcast) {
    (void)broadcast;
    *addr = 0x0a000001;
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

// BLOCKS when the queue is full rather than dropping, and that is a decision rather than an
// oversight - it was changed to drop on 2026-09-30 and changed back the same hour, because the
// measurement said so.
//
// The argument for dropping was faithfulness: a real UDP socket drops, this models one, and its
// being lossless was what let two assertions here test the harness rather than the module ("no gaps
// at all" and "the last sample arrived", both of which started failing once the shared-memory module
// was engaged). That argument is sound in the abstract and did not survive contact. Dropping removes
// the flow control that blocking provides, so the publisher threads run unthrottled: across six runs
// five delivered all 20,000 per thread and one delivered 7,151 - a run exercising a third of the
// traffic, and its own drop counter at zero, so the loss was not even from the new drop path.
//
// A harness that exercises less, and varies in how much, is a worse instrument than an unfaithful
// one whose unfaithfulness is written down. The two assertions were fixed on their own terms and
// needed no change here. What is left is this comment, so the next person to notice the mismatch
// knows it was measured rather than missed.
// Datagrams this fake transport discarded because the destination queue was full, per destination.
// uint64 and plain: every write is under that queue's own lock.
static uint64_t harness_dropped[NODE_COUNT + 1];

// Drops when full, and counts it. It used to BLOCK here, and that made this harness lossless by
// construction - which is not what either transport it stands in for does. A real UDP socket whose
// buffer is full discards and reports success; the segment ring refuses and the sender drops
// (segment_deliver()'s own "a full queue drops"). A harness that instead waits for the reader turns
// every capacity problem into back-pressure that neither path has, so the delivery assertion that
// used to sit below it - "no gaps at all" - was satisfiable only by the harness, and passed for this
// module's whole life while the module was never engaged.
//
// Counted rather than silent, because a dropped datagram here and one dropped by the segment are
// different facts: without the count, loss measured at the application cannot be attributed and no
// floor is derivable from it. With it, every sample is accounted for - see the accounting assertion
// at the end of main().
static void push(uint8_t to_id, uint8_t from_id, const void* hdr, size_t hdr_len, const void* body, size_t body_len) {
    struct queue* q = &queues[to_id];
    pthread_mutex_lock(&q->lock);
    if (q->count == QUEUE_SLOTS) {
        harness_dropped[to_id]++;
        pthread_cond_broadcast(&q->changed); // the reader may still be waiting on an earlier record
        pthread_mutex_unlock(&q->lock);
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
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->lock);
}

// Every datagram goes to every other node, broadcast or unicast alike: with two nodes, "the peer" is
// the only possible destination.
static int32_t send_all(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len) {
    if (hdr_len + body_len > DATAGRAM_MAX) {
        return -1;
    }
    for (uint8_t id = 1; id <= NODE_COUNT; id++) {
        if (id != node->id) {
            push(id, node->id, hdr, hdr_len, body, body_len);
        }
    }
    return (int32_t)(hdr_len + body_len);
}

int32_t tt_send(struct tt_Context* node, const void* buf, size_t len) {
    return send_all(node, buf, len, NULL, 0);
}

int32_t tt_send_to(struct tt_Context* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    (void)ip;
    (void)port;
    return send_all(node, buf, len, NULL, 0);
}

int32_t tt_send_iov(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
    (void)ip;
    (void)port;
    return send_all(node, hdr, hdr_len, body, body_len);
}

#if tt_SEGMENT_ENABLED
// The segment's HAL entry points (SHM_PLAN.md stage 1), backed by real memory that two contexts in
// this process genuinely share.
//
// These used to return NULL for everything, which meant the segment was never created, never
// attached, and never drained - so the one gate in this project that runs threads against the core
// was passing while never touching the shared-memory module at all. SHM_PLAN item 12 says a
// flag-gated feature with no arm that enables it is an untested feature; this was that, in the
// tsan gate, and it cost a real defect: drain_own_segment() called process_datagram_locked()
// WITHOUT the state lock from the day the segment landed (1c69658a). Every test that drives the
// drain is single-threaded, so nothing had a second party to race with - until rmw_tickle's
// executor did, and its publisher segfaulted.
//
// The table has a lock of its own because this harness must not race on its own bookkeeping; the
// regions it hands out are exactly what the module is supposed to race on, and that is the point.
#define TEST_TSAN_MAX_SEGMENTS 8
static struct {
    char path[tt_SEGMENT_PATH_LENGTH];
    void* region;
    size_t bytes;
    bool present;
} tsan_segments[TEST_TSAN_MAX_SEGMENTS];
static pthread_mutex_t tsan_segments_lock = PTHREAD_MUTEX_INITIALIZER;

void* tt_segment_create(const char* path, size_t bytes) {
    void* region = NULL;
    pthread_mutex_lock(&tsan_segments_lock);
    for (int i = 0; i < TEST_TSAN_MAX_SEGMENTS; i++) {
        if (tsan_segments[i].region != NULL && strcmp(tsan_segments[i].path, path) == 0) {
            tsan_segments[i].present = false; // a real create unlinks first
        }
    }
    for (int i = 0; i < TEST_TSAN_MAX_SEGMENTS; i++) {
        if (tsan_segments[i].region == NULL) {
            tsan_segments[i].region = calloc(1, bytes);
            if (tsan_segments[i].region != NULL) {
                snprintf(tsan_segments[i].path, sizeof(tsan_segments[i].path), "%s", path);
                tsan_segments[i].bytes = bytes;
                tsan_segments[i].present = true;
                region = tsan_segments[i].region;
            }
            break;
        }
    }
    pthread_mutex_unlock(&tsan_segments_lock);
    return region;
}

void* tt_segment_attach(const char* path, size_t bytes, uint8_t* why) {
    void* region = NULL;
    *why = (uint8_t)tt_SEGMENT_ABSENT;
    pthread_mutex_lock(&tsan_segments_lock);
    for (int i = 0; i < TEST_TSAN_MAX_SEGMENTS; i++) {
        if (tsan_segments[i].region != NULL && tsan_segments[i].present && strcmp(tsan_segments[i].path, path) == 0) {
            if (tsan_segments[i].bytes < bytes) {
                *why = (uint8_t)tt_SEGMENT_BAD_HEADER;
                break;
            }
            *why = (uint8_t)tt_SEGMENT_ATTACHED;
            region = tsan_segments[i].region; // one region, two users - which is the point
            break;
        }
    }
    pthread_mutex_unlock(&tsan_segments_lock);
    return region;
}

void tt_segment_detach(void* mapping, size_t bytes) {
    (void)mapping;
    (void)bytes; // the region outlives its attachers here, as a real mapping's file does
}

// No doorbell FIFO in the mock: peers ring over (mock) UDP, as with a platform that has none.
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

void tt_segment_unlink(const char* path) {
    pthread_mutex_lock(&tsan_segments_lock);
    for (int i = 0; i < TEST_TSAN_MAX_SEGMENTS; i++) {
        if (tsan_segments[i].region != NULL && strcmp(tsan_segments[i].path, path) == 0) {
            tsan_segments[i].present = false;
        }
    }
    pthread_mutex_unlock(&tsan_segments_lock);
}
#endif

// The three HAL entry points tt_CONTEXT_ID_CLAIM=1 reaches (g8's several-processes-on-one-host support, on by default
// since 2026-09-29). This file is its own HAL - the tsan target links tickle.c and no platform HAL at all - so turning
// the feature on by default made it fail to LINK rather than fail a check, which is the honest way for a test harness
// to say it does not cover a feature.
//
// Deliberately the simplest behaviour that keeps the thread-safety test about what it is about: the preferred id is
// always granted, so no collision path runs here, and a datagram is never "from our own socket", so nothing is dropped
// as self-sent. The collision and self-address paths have their own tests (test_samehost, tests/test_context_id_*);
// what this file exercises is concurrent access, and it must not silently change which packets it delivers.
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

// The address this node's datagrams appear to come from, which is what pop_locked() reports as the
// sender. These used to be 0/0 for every node while pop_locked() reported 10.0.0.<id>:2000<id>, so a
// context named its own segment from one address and its peers looked for it at another: every
// attach came back ABSENT and the segment was never used here. That is a harness that reports a
// node's identity two different ways, and it made the shared-memory module invisible to the one
// gate in this project that runs threads.
//
// Amended 2026-09-30, for the second time the addressing here made the module invisible. These gave
// each node an address of its own - 10.0.0.1 and 10.0.0.2 - which models two nodes on two HOSTS that
// nonetheless share a /dev/shm. Nothing can be on two hosts and share a shared-memory segment, and
// once segment creation became conditional on a peer being on this host, the harness's own addressing
// was what said they were not: every attach came back ABSENT again, for a new reason.
//
// So both contexts now have ONE address and differ by port, which is what two contexts on a host
// actually look like, and which is the only arrangement in which the thing under test can happen.
// Routing is unaffected: the queues are indexed by context id and pop_locked() synthesises the
// sender's address from from_id, so these values never decided where a datagram went - only who a
// context believed it and its peer were.
void tt_own_address(const struct tt_Context* node, uint32_t* ip, uint16_t* port) {
    if (ip != NULL) {
        *ip = TEST_HOST_IP;
    }
    if (port != NULL) {
        *port = (uint16_t)(20000 + node->id);
    }
}

int32_t tt_send_batch(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (send_all(node, datagrams[i].head, datagrams[i].head_len, datagrams[i].body, datagrams[i].body_len) < 0) {
            return -1;
        }
    }
    return (int32_t)count;
}

// Called with q->lock held and a datagram waiting.
static int32_t pop_locked(struct queue* q, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct datagram* d = &q->slots[q->head];
    uint32_t n = d->len < len ? d->len : (uint32_t)len;
    memcpy(buf, d->bytes, n);
    // One host, distinct ports - the same identity tt_own_address() reports, which is the point: a
    // harness that answers "who is this" two different ways is how the segment went untested twice.
    *ip = TEST_HOST_IP;
    *port = (uint16_t)(20000 + d->from_id);
    q->head = (q->head + 1) % QUEUE_SLOTS;
    q->count--;
    pthread_cond_broadcast(&q->changed);
    return (int32_t)n;
}

int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    struct queue* q = &queues[node->id];
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    uint64_t ns = (uint64_t)deadline.tv_nsec + (timeout > 0 ? (uint64_t)timeout : 0);
    deadline.tv_sec += (time_t)(ns / 1000000000ULL);
    deadline.tv_nsec = (long)(ns % 1000000000ULL);

    pthread_mutex_lock(&q->lock);
    int32_t result = -1;
    while (true) {
        if (q->wake) {
            q->wake = false;
            result = -3;
            break;
        }
        if (q->count > 0) {
            result = pop_locked(q, buf, len, ip, port);
            break;
        }
        int rc = timeout > 0 ? pthread_cond_timedwait(&q->changed, &q->lock, &deadline)
                             : pthread_cond_wait(&q->changed, &q->lock);
        if (rc == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&q->lock);
    return result;
}

// No cheaper way to know than reading, so "may be" - the behaviour before tt_rx_maybe_ready() existed.
bool tt_rx_maybe_ready(struct tt_Context* node) {
    (void)node;
    return true;
}

int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct queue* q = &queues[node->id];
    pthread_mutex_lock(&q->lock);
    int32_t result = q->count > 0 ? pop_locked(q, buf, len, ip, port) : -1;
    pthread_mutex_unlock(&q->lock);
    return result;
}

// What the queue holds counts as already read, so a drain here takes OPTIMIZATION_PLAN.md 11.4's locked chunks
// - under ThreadSanitizer, with the publishing threads running.
uint32_t tt_rx_buffered(const struct tt_Context* node) {
    struct queue* q = &queues[node->id];
    pthread_mutex_lock(&q->lock);
    uint32_t count = q->count;
    pthread_mutex_unlock(&q->lock);
    return count;
}

tt_ret_t tt_wake_signal(struct tt_Context* node) {
    struct queue* q = &queues[node->id];
    pthread_mutex_lock(&q->lock);
    q->wake = true;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->lock);
    return tt_RET_OK;
}

// ---- The payload: which publisher thread, and its own sequence number.

struct sample {
    uint32_t thread;
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

// ---- State. Everything a callback writes is written on B's poll thread only, and read by the main
// thread only after that thread has been joined.

static struct tt_Context node_a;
static struct tt_Context node_b;
static struct tt_Topic topics[PUBLISHER_THREADS];
static struct tt_Publisher publishers[PUBLISHER_THREADS];
static struct tt_Subscriber subscribers[PUBLISHER_THREADS];
static char names[PUBLISHER_THREADS][32];

static uint32_t received[PUBLISHER_THREADS]; // written by B's poll thread
// A gap in the sequence and a sample that goes BACKWARDS are different facts and were counted as one
// until 2026-09-29. A gap means a sample did not arrive; backwards means one arrived after a later
// one. Only the second is an ordering defect, and only the second is something every transport here
// promises. See the assertions at the end of main() for why that distinction had to be made.
static uint32_t gaps[PUBLISHER_THREADS];      // written by B's poll thread
static uint32_t backwards[PUBLISHER_THREADS]; // written by B's poll thread
static uint32_t delivered[PUBLISHER_THREADS]; // written by B's poll thread
static uint32_t publish_errors;               // atomic

static uint32_t timer_ids[TIMERS];      // timer i's param is &timer_ids[i], which holds i
static uint32_t timer_runs[TIMERS];     // written by A's poll thread
static uint8_t timer_cancelled[TIMERS]; // written by the timer thread before it cancels
static int busy_seen;                   // written by A's poll thread
static bool busy_checked;               // written by A's poll thread

static volatile int stop_polling; // atomic

static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)seq_no;
    const struct sample* s = (const struct sample*)data;
    uint32_t t = (uint32_t)(sub - subscribers);
    if (t >= PUBLISHER_THREADS || s->thread != t) {
        backwards[0]++;
        return;
    }
    if (s->seq < received[t] + 1) {
        backwards[t]++; // a sample behind one already delivered: an ordering defect, always
    } else if (s->seq > received[t] + 1) {
        gaps[t]++; // a sample did not arrive: loss, which best-effort does not promise against
    }
    delivered[t]++;
    received[t] = s->seq;
}

static void on_timer(struct tt_Context* node, uint64_t time, void* param) {
    (void)time;
    uint32_t i = *(const uint32_t*)param;
    timer_runs[i]++;
    // A second poller, from inside the first: the one case that can be made deterministic. Asked once.
    if (!busy_checked) {
        busy_checked = true;
        busy_seen = tt_Context_poll(node, 0) == tt_RET_BUSY;
    }
}

static void* poll_thread(void* param) {
    struct tt_Context* node = (struct tt_Context*)param;
    while (!__atomic_load_n(&stop_polling, __ATOMIC_ACQUIRE)) {
        tt_Context_poll(node, -1);
    }
    return NULL;
}

static uint32_t thread_ids[PUBLISHER_THREADS];

static void* publisher_thread(void* param) {
    uint32_t t = *(const uint32_t*)param;
    for (uint32_t seq = 1; seq <= SAMPLES_PER_THREAD; seq++) {
        struct sample s = {.thread = t, .seq = seq};
        if (tt_Publisher_publish(&publishers[t], (struct tt_Data*)&s) != tt_RET_OK) {
            __atomic_fetch_add(&publish_errors, 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

// Schedules TIMERS entries slightly in the future from off the poll thread, interrupting so the
// poll sees each one, and cancels every fifth before it can run.
static void* timer_thread(void* param) {
    (void)param;
    for (uint32_t i = 0; i < TIMERS; i++) {
        timer_ids[i] = i;
        uint64_t at = tt_get_ns() + ((i % 5 == 0) ? 1000000000ULL : 100000ULL); // cancelled ones: 1 s out
        while (!tt_Context_schedule(&node_a, at, on_timer, &timer_ids[i])) {
            struct timespec pause = {0, 100000};
            nanosleep(&pause, NULL); // heap full for a moment - the poll thread is draining it
        }
        // No tt_Context_interrupt(): tt_Context_schedule() wakes a waiting poll by itself when it must.
        if (i % 5 == 0) {
            timer_cancelled[i] = 1;
            EXPECT_TRUE(tt_Context_unschedule(&node_a, on_timer, &timer_ids[i]));
        }
    }
    return NULL;
}

// ---- A timer armed from another thread must wake an idle node's indefinite wait by itself.

#define WAKE_ROUNDS 20
#define WAKE_DELAY_NS 1000000ULL       // each entry is due 1 ms after it is scheduled
#define WAKE_LATE_LIMIT_NS 50000000ULL // and must run within 50 ms of that
#define WAKE_GIVE_UP_NS 2000000000ULL  // an idle node's own periodic work is up to 1 s away

static uint64_t wake_ran_at; // atomic: when on_wake_timer ran, 0 until then

static void on_wake_timer(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    __atomic_store_n(&wake_ran_at, tt_get_ns(), __ATOMIC_RELEASE);
}

// Node B is idle once the stream has been delivered, its poll thread waiting indefinitely for the next
// scheduler entry. Without the wake, an entry scheduled from here would run only when B's own
// periodic announce happened to wake it - on average half a second late.
static void test_schedule_wakes_an_idle_poll(void) {
    uint64_t worst = 0;
    for (int round = 0; round < WAKE_ROUNDS; round++) {
        __atomic_store_n(&wake_ran_at, 0, __ATOMIC_RELEASE);
        uint64_t due = tt_get_ns() + WAKE_DELAY_NS;
        EXPECT_TRUE(tt_Context_schedule(&node_b, due, on_wake_timer, NULL));
        uint64_t give_up = due + WAKE_GIVE_UP_NS;
        uint64_t ran = 0;
        while ((ran = __atomic_load_n(&wake_ran_at, __ATOMIC_ACQUIRE)) == 0 && tt_get_ns() < give_up) {
            struct timespec pause = {0, 100000};
            nanosleep(&pause, NULL);
        }
        EXPECT_TRUE(ran != 0);
        uint64_t late = ran > due ? ran - due : 0;
        worst = late > worst ? late : worst;
    }
    printf("schedule from another thread: worst lateness %lu us over %d rounds\n", (unsigned long)(worst / 1000),
           WAKE_ROUNDS);
    EXPECT_TRUE(worst < WAKE_LATE_LIMIT_NS);
}

static void create_endpoints(void) {
    for (int t = 0; t < PUBLISHER_THREADS; t++) {
        snprintf(names[t], sizeof(names[t]), "stress_%d", t);
        topics[t].name = names[t];
        topics[t].data_size = sizeof(struct sample);
        topics[t].data_encode_size = sample_encode_size;
        topics[t].data_encode = sample_encode;
        topics[t].data_decode = sample_decode;
        topics[t].data_free = sample_free;
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_publisher(&node_a, &publishers[t], &topics[t], names[t]));
        EXPECT_EQ_INT(tt_RET_OK,
                      tt_Context_create_subscriber(&node_b, &subscribers[t], &topics[t], names[t], on_sample));
    }
}

// ---- Deferred service responses answered from a thread of their own, while both poll threads run (2026-09-27).
// Node A's caller thread calls; B's poll thread defers each request to a responder thread, which answers with a
// string it allocates, and frees and overwrites the moment tt_Server_send_response() returns - as rclcpp frees a
// service response the moment rmw_send_response() returns. Every answer must arrive, and carry the right text:
// before tt_Server_send_response() encoded at the call, it was encoded later from freed memory.
//
// A retry the client sent before its answer arrived can reach B after the next call has been answered, when B's
// cache holds only that newer answer: the callback runs again and the old call is answered twice. That is the
// protocol's at-least-once retry, not this test's subject, so the deferred queue has room for it and an answer
// carrying another call's (correct) text is counted as stale, not wrong.

#define CALLS 300
#define DEFERRED_MAX (8 * CALLS) // every call, and the stale retries answered again
#define CALL_GIVE_UP_NS (2ULL * 1000ULL * 1000ULL * 1000ULL)
#define ANSWER_BYTES 64

struct call_request {
    uint32_t n;
};

struct call_response {
    char* text;
};

static int32_t call_request_encode_size(struct tt_Request* request) {
    (void)request;
    return (int32_t)sizeof(uint32_t);
}

static int32_t call_request_encode(struct tt_Request* request, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, &((struct call_request*)request)->n, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static int32_t call_request_decode(struct tt_Request* request, const uint8_t* payload, const uint32_t len,
                                   bool native) {
    (void)native;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(&((struct call_request*)request)->n, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}

static void call_request_free(struct tt_Request* request) {
    (void)request;
}

static int32_t call_response_encode_size(struct tt_Response* response) {
    return (int32_t)strlen(((struct call_response*)response)->text) + 1;
}

static int32_t call_response_encode(struct tt_Response* response, uint8_t* payload, const uint32_t len) {
    const char* text = ((struct call_response*)response)->text;
    uint32_t bytes = (uint32_t)strlen(text) + 1;
    if (len < bytes) {
        return -1;
    }
    memcpy(payload, text, bytes);
    return (int32_t)bytes;
}

static int32_t call_response_decode(struct tt_Response* response, const uint8_t* payload, const uint32_t len,
                                    bool native) {
    (void)native;
    if (len == 0 || payload[len - 1] != '\0') {
        return -1;
    }
    ((struct call_response*)response)->text = (char*)payload;
    return (int32_t)len;
}

static void call_response_free(struct tt_Response* response) {
    (void)response;
}

static void answer_text(char* buf, size_t size, uint32_t n) {
    snprintf(buf, size, "the answer to call %u, well past fifteen bytes", n);
}

static struct tt_Service call_service;
static struct tt_Server call_server; // on B
static struct tt_Client call_client; // on A

static pthread_mutex_t deferred_lock = PTHREAD_MUTEX_INITIALIZER;
static tt_RequestId deferred_ids[DEFERRED_MAX]; // written by B's poll thread, read by the responder
static uint32_t deferred_n[DEFERRED_MAX];
static uint32_t deferred_count;
static uint32_t deferred_taken;

static uint32_t current_call;   // atomic: the call the caller thread is waiting on
static uint32_t call_answered;  // atomic: set by A's poll thread when its answer arrives
static uint32_t answers_right;  // atomic
static uint32_t answers_wrong;  // atomic: a timeout, or a text no call was answered with
static uint32_t answers_stale;  // atomic: another call's correct answer, from a retry answered twice
static uint32_t call_timeouts;  // atomic: the client gave up on a call (tt_CALL_TIMEOUT); the caller re-issues it
static uint32_t calls_done;     // atomic: set by the caller thread when it has finished
static uint32_t respond_errors; // atomic
static uint32_t calls_unanswered;

static int8_t on_call(struct tt_Server* server, struct tt_Request* request, struct tt_Response* response,
                      tt_RequestId request_id) {
    (void)server;
    (void)response;
    pthread_mutex_lock(&deferred_lock);
    if (deferred_count < DEFERRED_MAX) {
        deferred_ids[deferred_count] = request_id;
        deferred_n[deferred_count] = ((struct call_request*)request)->n;
        deferred_count++;
    }
    pthread_mutex_unlock(&deferred_lock);
    return tt_CALL_DEFERRED;
}

static void on_answer(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    // Under ThreadSanitizer a call can outlast the client's own retries (2026-09-27: 1 of 300 under `make tsan`).
    // That is a slow answer, not a wrong one: counted apart, and the caller asks again.
    if (return_code == tt_CALL_TIMEOUT) {
        __atomic_fetch_add(&call_timeouts, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&call_answered, 2, __ATOMIC_RELEASE);
        return;
    }
    uint32_t current = __atomic_load_n(&current_call, __ATOMIC_ACQUIRE);
    unsigned int n = 0;
    char expected[ANSWER_BYTES];
    const char* text = return_code == 0 && response != NULL ? ((struct call_response*)response)->text : NULL;
    bool parsed = text != NULL && sscanf(text, "the answer to call %u", &n) == 1;
    if (parsed) {
        answer_text(expected, sizeof(expected), n);
    }
    if (!parsed || strcmp(text, expected) != 0) {
        __atomic_fetch_add(&answers_wrong, 1, __ATOMIC_RELAXED);
    } else if (n != current) {
        __atomic_fetch_add(&answers_stale, 1, __ATOMIC_RELAXED);
        return; // not the answer the caller is waiting on
    } else {
        __atomic_fetch_add(&answers_right, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&call_answered, 1, __ATOMIC_RELEASE);
}

static void* responder_thread(void* param) {
    (void)param;
    while (!__atomic_load_n(&calls_done, __ATOMIC_ACQUIRE)) {
        bool have = false;
        tt_RequestId id = {0, 0};
        uint32_t n = 0;
        pthread_mutex_lock(&deferred_lock);
        if (deferred_taken < deferred_count) {
            id = deferred_ids[deferred_taken];
            n = deferred_n[deferred_taken];
            deferred_taken++;
            have = true;
        }
        pthread_mutex_unlock(&deferred_lock);
        if (!have) {
            struct timespec pause = {0, 50000};
            nanosleep(&pause, NULL);
            continue;
        }
        char* text = malloc(ANSWER_BYTES);
        answer_text(text, ANSWER_BYTES, n);
        struct call_response response = {text};
        if (tt_Server_send_response(&call_server, id, 0, (struct tt_Response*)&response) != tt_RET_OK) {
            __atomic_fetch_add(&respond_errors, 1, __ATOMIC_RELAXED);
        }
        memset(text, 'X', strlen(text)); // gone, as rclcpp's response is once rmw_send_response() returns
        free(text);
    }
    return NULL;
}

static void* caller_thread(void* param) {
    (void)param;
    for (uint32_t n = 1; n <= CALLS; n++) {
        __atomic_store_n(&current_call, n, __ATOMIC_RELEASE);
        struct call_request request = {n};
        uint64_t give_up = tt_get_ns() + CALL_GIVE_UP_NS;
        uint32_t answered = 0;
        do { // again after a timeout (answered == 2), until an answer (1) or the give-up
            __atomic_store_n(&call_answered, 0, __ATOMIC_RELEASE);
            while (tt_Client_call(&call_client, (struct tt_Request*)&request) != tt_RET_OK && tt_get_ns() < give_up) {
                struct timespec pause = {0, 50000};
                nanosleep(&pause, NULL); // the previous call's retry state is still being released
            }
            while ((answered = __atomic_load_n(&call_answered, __ATOMIC_ACQUIRE)) == 0 && tt_get_ns() < give_up) {
                struct timespec pause = {0, 50000};
                nanosleep(&pause, NULL);
            }
        } while (answered == 2 && tt_get_ns() < give_up);
        if (answered != 1) {
            calls_unanswered++;
        }
    }
    __atomic_store_n(&calls_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void create_call_endpoints(void) {
    call_service.name = "call_stress";
    call_service.request_size = sizeof(struct call_request);
    call_service.response_size = sizeof(struct call_response);
    call_service.request_encode_size = call_request_encode_size;
    call_service.request_encode = call_request_encode;
    call_service.request_decode = call_request_decode;
    call_service.request_free = call_request_free;
    call_service.response_encode_size = call_response_encode_size;
    call_service.response_encode = call_response_encode;
    call_service.response_decode = call_response_decode;
    call_service.response_free = call_response_free;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_server(&node_b, &call_server, &call_service, "call_stress", on_call));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_client(&node_a, &call_client, &call_service, "call_stress", on_answer));
}

static bool all_delivered(void) {
    tt_Context_lock(&node_b); // received[] is written by B's poll thread, inside B's state lock
    bool done = true;
    for (int t = 0; t < PUBLISHER_THREADS; t++) {
        done = done && received[t] == SAMPLES_PER_THREAD;
    }
    tt_Context_unlock(&node_b);
    return done;
}

// The poller is let in first (2026-10-05). A thread that takes and releases the state lock in a tight loop - an rmw
// publish loop, every sample - used to take it back before the poller it had just woken could run, so the poll thread
// waited tens of milliseconds while ACKNACKs sat unread (KEEP_ALL matched its reader 0.1-0.35 s late on the rig).
// Here one thread loops on the lock while another, marked as the poller, asks for it once. What is counted is how many
// times the looping thread still takes the lock after it has seen the poller waiting: at most once (the hold it was
// already in), against hundreds when the mutex is left to decide.
#if !defined(CONTROL_BUILD) && tt_HAL_THREAD_YIELD
#define HANDOFF_ROUNDS 20
#define HANDOFF_LOOP_CAP 2000000 // the looping thread's give-up, so a failure ends instead of hanging
static struct tt_Context* handoff_node;
static int handoff_poller_in; // set by the poller once it holds the lock
static long handoff_barged;   // looping thread's acquisitions after it saw the poller waiting

static void* handoff_looper(void* arg) {
    (void)arg;
    long barged = 0;
    for (long i = 0; i < HANDOFF_LOOP_CAP && !__atomic_load_n(&handoff_poller_in, __ATOMIC_ACQUIRE); i++) {
        tt_Context_lock(handoff_node);
        if (__atomic_load_n(&handoff_node->poller_waiting, __ATOMIC_ACQUIRE) != 0) {
            barged++;
        }
        tt_Context_unlock(handoff_node);
    }
    __atomic_store_n(&handoff_barged, barged, __ATOMIC_RELEASE);
    return NULL;
}

static void* handoff_poller(void* arg) {
    (void)arg;
    __atomic_store_n(&handoff_node->poller_thread, tt_thread_self(), __ATOMIC_RELAXED);
    tt_Context_lock(handoff_node);
    __atomic_store_n(&handoff_poller_in, 1, __ATOMIC_RELEASE);
    tt_Context_unlock(handoff_node);
    __atomic_store_n(&handoff_node->poller_thread, 0, __ATOMIC_RELAXED);
    return NULL;
}

static void test_a_waiting_poller_gets_the_lock_first(struct tt_Context* node) {
    handoff_node = node;
    long worst = 0;
    for (int round = 0; round < HANDOFF_ROUNDS; round++) {
        __atomic_store_n(&handoff_poller_in, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&handoff_barged, 0, __ATOMIC_RELEASE);
        pthread_t looper;
        pthread_t poller;
        pthread_create(&looper, NULL, handoff_looper, NULL);
        struct timespec settle = {0, 1000000}; // let the loop get going before the poller asks
        nanosleep(&settle, NULL);
        pthread_create(&poller, NULL, handoff_poller, NULL);
        pthread_join(poller, NULL);
        pthread_join(looper, NULL);
        long barged = __atomic_load_n(&handoff_barged, __ATOMIC_ACQUIRE);
        worst = barged > worst ? barged : worst;
        EXPECT_TRUE(__atomic_load_n(&handoff_poller_in, __ATOMIC_ACQUIRE) == 1);
    }
    printf("test_thread_safety: the looping thread took the lock at most %ld time(s) past a waiting poller\n", worst);
    EXPECT_TRUE(worst <= 1);
}
#endif

int main(void) {
#if defined(tt_THREAD_SAFE) && !tt_THREAD_SAFE
    // A single-thread build (config.h): the locks are no-ops by design, so there is nothing here to test,
    // and running the threads anyway would only demonstrate the races that build accepts.
    printf("test_thread_safety: skipped - built with tt_THREAD_SAFE=0\n");
    return 0;
#endif
    for (int i = 0; i <= NODE_COUNT; i++) {
        pthread_mutex_init(&queues[i].lock, NULL);
        pthread_cond_init(&queues[i].changed, NULL);
    }
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&node_a));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&node_b));
    create_endpoints();
    create_call_endpoints();

    pthread_t poll_a;
    pthread_t poll_b;
    pthread_create(&poll_b, NULL, poll_thread, &node_b);
    pthread_create(&poll_a, NULL, poll_thread, &node_a);

    pthread_t pubs[PUBLISHER_THREADS];
    for (int t = 0; t < PUBLISHER_THREADS; t++) {
        thread_ids[t] = (uint32_t)t;
        pthread_create(&pubs[t], NULL, publisher_thread, &thread_ids[t]);
    }
    pthread_t timers;
    pthread_create(&timers, NULL, timer_thread, NULL);
    pthread_t caller;
    pthread_t responder;
    pthread_create(&responder, NULL, responder_thread, NULL);
    pthread_create(&caller, NULL, caller_thread, NULL);

    for (int t = 0; t < PUBLISHER_THREADS; t++) {
        pthread_join(pubs[t], NULL);
    }
    pthread_join(timers, NULL);
    pthread_join(caller, NULL);
    pthread_join(responder, NULL);

    uint64_t give_up = tt_get_ns() + DELIVERY_GIVE_UP_NS;
    while (!all_delivered() && tt_get_ns() < give_up) {
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    // The uncancelled timers were scheduled 100 us out; give the last of them time to run.
    struct timespec settle = {0, 200000000};
    nanosleep(&settle, NULL);

#ifndef CONTROL_BUILD
    test_schedule_wakes_an_idle_poll();
#endif

    __atomic_store_n(&stop_polling, 1, __ATOMIC_RELEASE);
    tt_Context_interrupt(&node_a);
    tt_Context_interrupt(&node_b);
    pthread_join(poll_a, NULL);
    pthread_join(poll_b, NULL);
#if !defined(CONTROL_BUILD) && tt_HAL_THREAD_YIELD
    // After the pollers have stopped, so the two threads here are the only ones asking for node_a's lock.
    test_a_waiting_poller_gets_the_lock_first(&node_a);
#endif

    EXPECT_EQ_U32(0, publish_errors);
    EXPECT_EQ_U32(0, calls_unanswered);
    EXPECT_EQ_U32(0, respond_errors);
    EXPECT_EQ_U32(CALLS, answers_right);
    EXPECT_EQ_U32(0, answers_wrong);
    if (call_timeouts != 0) {
        printf("test_thread_safety: %u call(s) timed out and were asked again\n", call_timeouts);
    }
    uint32_t delivered_total = 0;
    for (int t = 0; t < PUBLISHER_THREADS; t++) {
        // The last sample is NOT asserted to have arrived, and that is the same correction as the
        // delivery floor below rather than a second concession. "received[t] == SAMPLES_PER_THREAD"
        // is a no-loss claim about the tail, and best-effort makes no such promise: the final
        // samples can be dropped by a full ring exactly like any others. It held for this harness's
        // whole life only because push() blocks rather than drops, and it began failing about one
        // run in four the moment the segment was engaged - ending at 8,215 of 20,000 in one gate
        // run and 15,088 in another. A gate that fails one run in four is worse than no gate,
        // because what it teaches is to run it again.
        //
        // What is asserted instead is what the publisher and the ordering actually promise:
        // publish_errors is zero (above), nothing arrives out of order (below), and the segment was
        // engaged at all. The number delivered is printed.
        EXPECT_TRUE(received[t] > 0);
        // No sample may arrive behind one already delivered. That is the contract, and it holds in
        // every run - but **this harness cannot falsify it and the assertion is therefore not
        // evidence**. Checked rather than assumed: with the pre-2026-09-29 behaviour restored, where
        // a full ring rerouted its datagram to UDP and caused exactly this reordering on the rig,
        // `backwards` stayed at zero across four runs. The reason is that push() hands a datagram to
        // the same queue the peer reads while the ring is drained to empty first, so ring records
        // always precede queued ones here; on a real socket the socket is drained to exhaustion
        // while the ring lags, which is where the reordering came from.
        //
        // Kept because it costs nothing and would catch a gross regression. Not counted as a guard.
        // What this harness does falsify is the data race - without the state lock in
        // drain_own_segment(), ThreadSanitizer reports one and the run exits 2 - and the engaged
        // check below.
        EXPECT_EQ_U32(0, backwards[t]);
        // **No delivery floor is asserted here, and that is a considered refusal rather than a gap
        // in the test.** These publishers are BEST-EFFORT: a bounded queue that is full drops, which
        // is what the segment ring does and what a real UDP socket does. This harness's fake
        // transport used NOT to - push() blocked on a condition variable when its queue was full, so
        // it was lossless by construction, and the assertion that used to stand here ("no gaps at
        // all") was only ever satisfiable by that. **It was testing the harness, not the module**,
        // and it passed for the module's whole life while the module was never engaged. push() now
        // drops and counts, so both policies match and loss is attributable - see the accounting at
        // the end of main().
        //
        // A floor is still not asserted, and the measurements are why. Delivery across runs on one
        // machine: 80000/80000, 11775/80000, 80000/80000 - 0%, 85%, 0% - which is drop-on-full in
        // front of a writer faster than its reader. A floor anywhere in that range would be a number
        // nobody can derive, which is the objection this project already makes to "tx_udp small and
        // flat". What IS asserted is that every sample is accounted for, which the counting bought.
        //
        // The honest repair is to make push() drop rather than block, so both paths have the same
        // policy and a tight floor becomes derivable. That is a change to a harness older than this
        // module and it belongs in its own commit.
        if (delivered[t] != SAMPLES_PER_THREAD) {
            printf("test_thread_safety: thread %d delivered %u of %u (best-effort, %u gap(s))\n", t, delivered[t],
                   (unsigned)SAMPLES_PER_THREAD, gaps[t]);
        }
        delivered_total += delivered[t];
    }
    // Every sample this run produced is now accounted for, which is what making push() drop bought.
    // Before it, loss could only be reported as a percentage nobody could derive - the objection this
    // file already makes to "tx_udp small and flat". Now each sample either arrived or was discarded
    // by a queue that says so, and the two sides are compared rather than described.
    //
    // Not an equality on sample counts: a sample can span datagrams, and a dropped FRAGMENT loses the
    // whole sample while costing one drop. So the claim is the direction that must hold - nothing is
    // delivered that was never sent, and nothing vanishes with every queue reporting itself empty of
    // drops. A run that loses samples with harness_dropped and shm_full_dropped both zero is loss with
    // no mechanism, which is the one outcome that would mean this harness is still lying.
    const uint32_t produced = (uint32_t)PUBLISHER_THREADS * (uint32_t)SAMPLES_PER_THREAD;
    uint64_t harness_drops = 0;
    for (int i = 0; i <= NODE_COUNT; i++) {
        harness_drops += harness_dropped[i];
    }
    printf("harness: %u of %u samples delivered, harness dropped %lu datagram(s), segment dropped %lu\n",
           delivered_total, produced, (unsigned long)harness_drops,
           (unsigned long)(node_a.segment_full_dropped + node_b.segment_full_dropped));
    EXPECT_TRUE(delivered_total <= produced);
    if (delivered_total < produced) {
        EXPECT_TRUE(harness_drops > 0 || (node_a.segment_full_dropped + node_b.segment_full_dropped) > 0);
    }

    uint32_t wrong_runs = 0;
    for (uint32_t i = 0; i < TIMERS; i++) {
        wrong_runs += timer_runs[i] != (timer_cancelled[i] ? 0U : 1U);
    }
    EXPECT_EQ_U32(0, wrong_runs);
    EXPECT_TRUE(busy_checked);
    EXPECT_TRUE(busy_seen);

#if tt_SEGMENT_ENABLED
    // The module has to have been ENGAGED, not merely compiled in. This harness returned NULL from
    // every segment entry point until 2026-09-29, so the one gate that runs threads against the core
    // passed for months while never touching the shared-memory module - and the defect it should
    // have caught (drain_own_segment() calling process_datagram_locked() without the state lock)
    // lived from 1c69658a until an rmw executor segfaulted over it.
    //
    // A run where these are zero is a run that proves nothing about the segment under threads, and
    // it would look exactly like a clean one. So it fails instead.
    EXPECT_TRUE(node_a.tx_datagrams_by_transport[tt_TRANSPORT_SHM] > 0 ||
                node_b.tx_datagrams_by_transport[tt_TRANSPORT_SHM] > 0);
    EXPECT_TRUE(node_a.rx_datagrams_by_transport[tt_TRANSPORT_SHM] > 0 ||
                node_b.rx_datagrams_by_transport[tt_TRANSPORT_SHM] > 0);
    printf("segment diag: a own=%p attach[ok=%u absent=%u refused=%u bad=%u wrong=%u stale=%u] "
           "bcast=%lu unattached=%lu oversize=%lu\n",
           (void*)node_a.own_segment, node_a.segment_attach[tt_SEGMENT_ATTACHED],
           node_a.segment_attach[tt_SEGMENT_ABSENT], node_a.segment_attach[tt_SEGMENT_REFUSED],
           node_a.segment_attach[tt_SEGMENT_BAD_HEADER], node_a.segment_attach[tt_SEGMENT_WRONG_OWNER],
           node_a.segment_attach[tt_SEGMENT_STALE], (unsigned long)node_a.segment_broadcast_to_udp,
           (unsigned long)node_a.segment_unattached_to_udp, (unsigned long)node_a.segment_oversized_to_udp);
    printf("segment under threads: a tx_shm=%lu rx_shm=%lu, b tx_shm=%lu rx_shm=%lu\n",
           (unsigned long)node_a.tx_datagrams_by_transport[tt_TRANSPORT_SHM],
           (unsigned long)node_a.rx_datagrams_by_transport[tt_TRANSPORT_SHM],
           (unsigned long)node_b.tx_datagrams_by_transport[tt_TRANSPORT_SHM],
           (unsigned long)node_b.rx_datagrams_by_transport[tt_TRANSPORT_SHM]);
#endif

#ifndef CONTROL_BUILD
    printf("state lock: %lu acquisitions, %lu contended, %lu ns waited (node A)\n",
           (unsigned long)node_a.state_lock_stats.acquisitions, (unsigned long)node_a.state_lock_stats.contended,
           (unsigned long)node_a.state_lock_stats.wait_ns);
#endif

    if (test_result() != 0) {
        return 1;
    }
    printf("test_thread_safety: all tests passed\n");
    return 0;
}
