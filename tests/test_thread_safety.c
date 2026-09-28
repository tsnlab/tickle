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

static void push(uint8_t to_id, uint8_t from_id, const void* hdr, size_t hdr_len, const void* body, size_t body_len) {
    struct queue* q = &queues[to_id];
    pthread_mutex_lock(&q->lock);
    while (q->count == QUEUE_SLOTS) {
        pthread_cond_wait(&q->changed, &q->lock);
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

void tt_own_address(const struct tt_Context* node, uint32_t* ip, uint16_t* port) {
    (void)node;
    if (ip != NULL) {
        *ip = 0;
    }
    if (port != NULL) {
        *port = 0;
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
    *ip = 0x0a000000U + d->from_id;
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

static uint32_t received[PUBLISHER_THREADS];     // written by B's poll thread
static uint32_t out_of_order[PUBLISHER_THREADS]; // written by B's poll thread
static uint32_t publish_errors;                  // atomic

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
        out_of_order[0]++;
        return;
    }
    if (s->seq != received[t] + 1) {
        out_of_order[t]++;
    }
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

    EXPECT_EQ_U32(0, publish_errors);
    EXPECT_EQ_U32(0, calls_unanswered);
    EXPECT_EQ_U32(0, respond_errors);
    EXPECT_EQ_U32(CALLS, answers_right);
    EXPECT_EQ_U32(0, answers_wrong);
    if (call_timeouts != 0) {
        printf("test_thread_safety: %u call(s) timed out and were asked again\n", call_timeouts);
    }
    for (int t = 0; t < PUBLISHER_THREADS; t++) {
        EXPECT_EQ_U32(SAMPLES_PER_THREAD, received[t]);
        EXPECT_EQ_U32(0, out_of_order[t]);
    }
    uint32_t wrong_runs = 0;
    for (uint32_t i = 0; i < TIMERS; i++) {
        wrong_runs += timer_runs[i] != (timer_cancelled[i] ? 0U : 1U);
    }
    EXPECT_EQ_U32(0, wrong_runs);
    EXPECT_TRUE(busy_checked);
    EXPECT_TRUE(busy_seen);

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
