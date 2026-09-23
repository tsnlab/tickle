/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Phase 3 step 3 (rmw_tickle/PLAN.md) - rmw_publish()'s own KEEP_ALL back-pressure. A KEEP_ALL
// Publisher refuses a write (tt_RET_WOULD_BLOCK) rather than dropping a sample no matched Subscriber
// has acknowledged yet, and rmw_publish() turns that refusal into a bounded wait: it blocks up to
// RMW_TICKLE_MAX_BLOCKING_MS and then reports RMW_RET_TIMEOUT. What this file pins down is that a
// full publisher *stalls and reports*, rather than the two failure modes that would actually hurt -
// silently dropping the sample, or hanging forever.
//
// test_history_keep_all.c already covers the create-time half (cache sizing, keep_all wiring). This
// is the runtime half.
//
// No second process, matching every other test in this package: a matched-but-stalled Subscriber is
// faked by claiming a peer_acks[] entry directly and never advancing its ack_seq_no. That is exactly
// the state a real never-acknowledging Subscriber produces (struct tt_Publisher.peer_acks' own doc
// comment, tickle.h), reached without needing a second node to misbehave on cue.

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <tickle/config.h> // tt_MILLISECOND, tt_RELIABLE_BITMAP_WORD_BITS
#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE, tt_NODE_ID_INVALID

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

// Must match RMW_TICKLE_KEEP_ALL_DEPTH_VOLATILE (rmw_publisher.c) and the bound it implies. The
// blocking bound is keep_all_bound() = min(cache depth, smallest announced tracking window), and
// the fake peer below announces RMW_TICKLE_TRACKING_WORDS words, so the window is what binds.
#define EXPECTED_BLOCK_AFTER ((uint32_t)RMW_TICKLE_TRACKING_WORDS * tt_RELIABLE_BITMAP_WORD_BITS)

// Deliberately short: this test spends real wall-clock time waiting for these to expire, three
// times over, and the point being proven is "bounded and reported", not the specific default.
#define TEST_MAX_BLOCKING_MS 40
// A wait that ends by timing out must actually have waited. Checked with a generous floor rather
// than an exact figure - pthread_cond_timedwait() may return slightly early relative to a separate
// clock reading, and a CI runner under load can be late by far more than it is ever early.
#define MIN_CREDIBLE_WAIT_MS 20
// ...and must not have waited absurdly longer, which is what a lost wakeup or a missing deadline
// would look like. Loose enough to survive a loaded shared runner.
#define MAX_CREDIBLE_WAIT_MS 4000

// Case 3 wakes the blocked write well before this, so a pass can only come from the wakeup path.
#define RECOVERING_MAX_BLOCKING_MS "2000"
#define UNBLOCK_DELAY_MS 30

// Stand-in identifiers for the faked matched Subscriber below - any values peer_acks[] reads as
// "in use" will do; nothing here decodes them.
#define FAKE_PEER_NODE_ID 42
#define FAKE_PEER_ENTITY_ID 1
#define FAKE_PAYLOAD 7

#define MS_PER_SEC 1000ULL
#define NS_PER_MS 1000000ULL

struct fake_ros_msg {
    uint8_t value;
};
struct fake_tickle_msg {
    uint8_t value;
};

static bool fake_to_tickle(const void* ros_message, void* tickle_message) {
    ((struct fake_tickle_msg*)tickle_message)->value = ((const struct fake_ros_msg*)ros_message)->value;
    return true;
}
static bool fake_from_tickle(const void* tickle_message, void* ros_message) {
    ((struct fake_ros_msg*)ros_message)->value = ((const struct fake_tickle_msg*)tickle_message)->value;
    return true;
}
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint8_t);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    payload[0] = ((struct fake_tickle_msg*)data)->value;
    return (int32_t)sizeof(uint8_t);
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)len;
    (void)is_native_endian;
    ((struct fake_tickle_msg*)data)->value = payload[0];
    return (int32_t)sizeof(uint8_t);
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t fake_callbacks = {
    .ros_type_name = "test_keep_all_blocking/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_tickle_msg),
    .ros_struct_size = sizeof(struct fake_ros_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_to_tickle,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_from_tickle,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};

static rosidl_message_type_support_t fake_handle = {
    .data = &fake_callbacks,
    .func = get_message_typesupport_handle_function,
    .get_type_hash_func = NULL,
    .get_type_description_func = NULL,
    .get_type_description_sources_func = NULL,
};

static const rosidl_message_type_support_t* fake_type_support(void) {
    if (NULL == fake_handle.typesupport_identifier) {
        fake_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    }
    return &fake_handle;
}

static rmw_qos_profile_t keep_all_reliable_qos(void) {
    rmw_qos_profile_t qos;
    qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
    qos.depth = 10; // ignored once KEEP_ALL is set - test_history_keep_all.c pins that
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    qos.deadline = (rmw_time_t)RMW_QOS_DEADLINE_DEFAULT;
    qos.lifespan = (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    qos.liveliness_lease_duration = (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT;
    qos.avoid_ros_namespace_conventions = false;
    return qos;
}

// Deliberately CLOCK_MONOTONIC, unlike rmw_publish()'s own deadline: this measures how long a call
// really took, which must not move if wall time is adjusted mid-test.
static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now); // NOLINT(misc-include-cleaner) - <time.h> above; see rmw_wait_set.c
    return ((uint64_t)now.tv_sec * MS_PER_SEC) + ((uint64_t)now.tv_nsec / NS_PER_MS);
}

// A matched Subscriber that has acknowledged nothing. node_id just has to be a value peer_acks[]
// reads as "in use"; tracking_words is what the real thing announces (rmw_create_subscription()
// sets RMW_TICKLE_TRACKING_WORDS), and is what sets the blocking bound.
static void attach_stalled_peer(rmw_tickle_publisher_t* pub_impl) {
    pub_impl->tickle_publisher.peer_acks[0].node_id = FAKE_PEER_NODE_ID;
    pub_impl->tickle_publisher.peer_acks[0].entity_id = FAKE_PEER_ENTITY_ID;
    pub_impl->tickle_publisher.peer_acks[0].ack_seq_no = 0;
    pub_impl->tickle_publisher.peer_acks[0].tracking_words = RMW_TICKLE_TRACKING_WORDS;
}

// Publishes until the Publisher refuses, returning how many writes were accepted first. Bounded so
// a regression that never blocks fails the assert below instead of looping forever.
static uint32_t fill_until_blocked(rmw_publisher_t* pub, rmw_ret_t* final_ret) {
    struct fake_ros_msg msg = {.value = FAKE_PAYLOAD};
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < EXPECTED_BLOCK_AFTER * 4; i++) {
        rmw_ret_t ret = rmw_publish(pub, &msg, NULL);
        if (RMW_RET_OK != ret) {
            *final_ret = ret;
            return accepted;
        }
        accepted++;
    }
    *final_ret = RMW_RET_OK;
    return accepted;
}

struct unblocker_args {
    rmw_tickle_publisher_t* pub_impl;
    uint32_t delay_ms;
    bool took_node_mutex;
};

// The thread that proves rmw_publish() is not holding node_mutex while it waits. It does exactly
// what the poll thread would do on a real incoming ACKNACK: take node_mutex, advance the slowest
// peer's ack_seq_no, and fire the Publisher's own writable callback under that lock.
//
// If rmw_publish() ever went back to waiting with node_mutex held, this thread would block on
// pthread_mutex_lock() forever and the test would hang rather than fail - which is why the caller
// also asserts on elapsed time, so the deadlock shows up as a bounded failure in CI.
static void* unblocker_main(void* param) {
    struct unblocker_args* args = (struct unblocker_args*)param;
    struct timespec nap = {.tv_sec = 0, .tv_nsec = (long)args->delay_ms * (long)tt_MILLISECOND};
    nanosleep(&nap, NULL);

    rmw_tickle_context_impl_t* context_impl = args->pub_impl->node->context_impl;
    pthread_mutex_lock(&context_impl->node_mutex);
    args->took_node_mutex = true;
    // Far enough that the blocked write, and plenty after it, fit under the bound again.
    args->pub_impl->tickle_publisher.peer_acks[0].ack_seq_no = args->pub_impl->tickle_publisher.seq_no;
    args->pub_impl->tickle_publisher.writable_callback(&args->pub_impl->tickle_publisher,
                                                       args->pub_impl->tickle_publisher.writable_callback_param);
    pthread_mutex_unlock(&context_impl->node_mutex);
    return NULL;
}

int main(void) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();

    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);

    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));

    rmw_node_t* node = rmw_create_node(&context, "test_keep_all_blocking", "/");
    assert(NULL != node);

    const rosidl_message_type_support_t* type_support = fake_type_support();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_qos_profile_t qos = keep_all_reliable_qos();

    // Case 1: with no matched Subscriber there is nothing to wait for, so KEEP_ALL must not block
    // at all - a publisher nobody is listening to has to keep running at full speed. Guards against
    // the obvious over-correction, where every KEEP_ALL write stalls for the timeout.
    {
        char env[8];
        snprintf(env, sizeof(env), "%d", TEST_MAX_BLOCKING_MS);
        setenv("RMW_TICKLE_MAX_BLOCKING_MS", env, 1);
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "keep_all_unmatched", &qos, &pub_opts);
        assert(NULL != pub);

        struct fake_ros_msg msg = {.value = 1};
        uint64_t started = monotonic_ms();
        for (uint32_t i = 0; i < EXPECTED_BLOCK_AFTER * 2; i++) {
            assert(RMW_RET_OK == rmw_publish(pub, &msg, NULL));
        }
        uint64_t elapsed = monotonic_ms() - started;
        assert(elapsed < MAX_CREDIBLE_WAIT_MS);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // Case 2: a matched-but-stalled Subscriber. Writes are accepted up to the bound and then the
    // publisher blocks and reports RMW_RET_TIMEOUT - it must neither drop the sample silently
    // (which returning RMW_RET_OK here would mean) nor wait forever.
    {
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "keep_all_stalled", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert((uint64_t)TEST_MAX_BLOCKING_MS * tt_MILLISECOND == pub_impl->max_blocking_ns);
        attach_stalled_peer(pub_impl);

        rmw_ret_t final_ret = RMW_RET_OK;
        uint64_t started = monotonic_ms();
        uint32_t accepted = fill_until_blocked(pub, &final_ret);
        uint64_t elapsed = monotonic_ms() - started;

        assert(RMW_RET_TIMEOUT == final_ret);
        assert(EXPECTED_BLOCK_AFTER == accepted);
        assert(elapsed >= MIN_CREDIBLE_WAIT_MS); // it really waited
        assert(elapsed < MAX_CREDIBLE_WAIT_MS);  // and really stopped
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // Case 3: the same stall, but the slowest peer catches up mid-wait. The blocked write must
    // then succeed rather than time out - and, critically, the thread delivering that progress has
    // to be able to take node_mutex while rmw_publish() is waiting. See unblocker_main().
    //
    // Runs with blocking set well above the unblock delay so a pass can only come from the wakeup
    // path, never from the deadline expiring and the retry happening to find room.
    {
        setenv("RMW_TICKLE_MAX_BLOCKING_MS", RECOVERING_MAX_BLOCKING_MS, 1);
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "keep_all_recovering", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
        attach_stalled_peer(pub_impl);

        struct fake_ros_msg msg = {.value = 3};
        for (uint32_t i = 0; i < EXPECTED_BLOCK_AFTER; i++) {
            assert(RMW_RET_OK == rmw_publish(pub, &msg, NULL)); // fill it right up to the bound
        }

        struct unblocker_args args = {.pub_impl = pub_impl, .delay_ms = UNBLOCK_DELAY_MS, .took_node_mutex = false};
        pthread_t unblocker; // NOLINT(misc-include-cleaner) - <pthread.h> above; glibc declares it via a private header
        assert(0 == pthread_create(&unblocker, NULL, unblocker_main, &args));

        uint64_t started = monotonic_ms();
        rmw_ret_t ret = rmw_publish(pub, &msg, NULL); // must block, then be woken, then succeed
        uint64_t elapsed = monotonic_ms() - started;
        assert(0 == pthread_join(unblocker, NULL));

        assert(args.took_node_mutex); // node_mutex was reachable while rmw_publish() waited
        assert(RMW_RET_OK == ret);
        assert(elapsed < MAX_CREDIBLE_WAIT_MS);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    // Case 4: RMW_TICKLE_MAX_BLOCKING_MS=0 means never block - refuse immediately. For a caller
    // that would rather drop a sample than stall its own loop, the timeout has to be reported
    // without waiting at all.
    {
        setenv("RMW_TICKLE_MAX_BLOCKING_MS", "0", 1);
        rmw_publisher_t* pub = rmw_create_publisher(node, type_support, "keep_all_nonblocking", &qos, &pub_opts);
        assert(NULL != pub);
        rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
        assert(0 == pub_impl->max_blocking_ns);
        attach_stalled_peer(pub_impl);

        rmw_ret_t final_ret = RMW_RET_OK;
        uint64_t started = monotonic_ms();
        uint32_t accepted = fill_until_blocked(pub, &final_ret);
        uint64_t elapsed = monotonic_ms() - started;

        assert(RMW_RET_TIMEOUT == final_ret);
        assert(EXPECTED_BLOCK_AFTER == accepted);
        assert(elapsed < MAX_CREDIBLE_WAIT_MS);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }

    unsetenv("RMW_TICKLE_MAX_BLOCKING_MS");
    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    printf("test_keep_all_blocking: PASS\n");
    return 0;
}
