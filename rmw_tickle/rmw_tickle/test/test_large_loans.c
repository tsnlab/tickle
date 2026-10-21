/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Loaned messages of a type larger than a datagram (docs/RMW.md, "Loaned messages"; core DESIGN.md section 8).
//
// A large sample is put together in a buffer of its own, which core lends (tt_LEND_LARGE). Before 2026-10-11 the
// subscription of a type above tt_MAX_BUFFER_LENGTH could not lend, so a 4 MB perf_test Array4m was decoded into a
// shell in the delivery and copied again by the take: two copies after its last fragment, where a DDS makes one (the
// rig's I4 cell, 1.2 ms behind Fast DDS). Now the delivery keeps the sample where it was assembled.
//
// Two processes (a forked talker, its own rmw context), an Array1m-shaped type (byte[1048576], int64, uint64 - its wire
// bytes are its struct), RELIABLE KEEP_LAST, the talker's samples sent over UDP (the listener's ring slots are too
// small for a fragment record):
//   loaned  taken with rmw_take_loaned_message: every sample whole and in order, every one lent where it was
//           assembled (loans_in_place, not a decoded shell), and core retained each one (lend.retains);
//   take    taken with rmw_take into the caller's message: the same samples, each retained by core in the delivery
//           and decoded once, by the take;
//   off     RMW_TICKLE_LOANS=0, the control: the subscription does not lend and core retains nothing, so the counters
//           the two cases above read are not what any delivery would show.

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define ARRAY1M_BYTES ((size_t)1024 * 1024)
#define SAMPLES 20U
#define DEPTH 10U
#define PACE_US 20000L // between publishes: a 1 MB sample is 723 datagrams, and every one must be taken
#define POLL_US 200L
#define WAIT_POLLS 50000 // 10 s at POLL_US
#define NS_PER_US 1000L
#define FILL_STEP 13U         // array[i] of sample `id` is (id * FILL_STEP + i) mod 256
#define TIME_PER_ID 1000U     // and its time is id * TIME_PER_ID
#define SMALL_SLOT_BYTES 512U // a listener ring slot a fragment record does not fit: the talker sends over UDP

// perf_test's Array1m: byte[1048576], int64, uint64.
struct array1m {
    uint8_t array[ARRAY1M_BYTES];
    int64_t time;
    uint64_t id;
};

static int32_t array1m_encode_size(const void* ros_message) {
    (void)ros_message;
    return (int32_t)sizeof(struct array1m);
}

static int32_t array1m_encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    if (len < sizeof(struct array1m)) {
        return -1;
    }
    memcpy(payload, ros_message, sizeof(struct array1m));
    return (int32_t)sizeof(struct array1m);
}

static int32_t array1m_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct array1m)) {
        return -1;
    }
    memcpy(ros_message, payload, sizeof(struct array1m));
    return (int32_t)sizeof(struct array1m);
}

static int32_t refuse_encode_size(struct tt_Data* data) {
    (void)data;
    return -1;
}

static rosidl_typesupport_tickle_c_message_callbacks_t array1m_callbacks = {
    .ros_type_name = "test_large_loans/msg/Array1m",
    .tickle_struct_size = sizeof(struct array1m),
    .ros_struct_size = sizeof(struct array1m),
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&refuse_encode_size,
    .tickle_max_encoded_size = sizeof(struct array1m),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
    .direct_encode_size = &array1m_encode_size,
    .direct_encode = &array1m_encode,
    .direct_decode = &array1m_decode,
    .inplace_bytes = sizeof(struct array1m),
    .ros_struct_align = _Alignof(struct array1m),
};

static rosidl_message_type_support_t array1m_handle = {.data = &array1m_callbacks,
                                                       .func = get_message_typesupport_handle_function};

static void fill(struct array1m* message, uint64_t id) {
    for (size_t i = 0; i < ARRAY1M_BYTES; i++) {
        message->array[i] = (uint8_t)((id * FILL_STEP) + i);
    }
    message->time = (int64_t)(id * TIME_PER_ID);
    message->id = id;
}

static bool intact(const struct array1m* message) {
    for (size_t i = 0; i < ARRAY1M_BYTES; i++) {
        if (message->array[i] != (uint8_t)((message->id * FILL_STEP) + i)) {
            return false;
        }
    }
    return message->time == (int64_t)(message->id * TIME_PER_ID);
}

static void sleep_us(long micros) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = micros * NS_PER_US};
    nanosleep(&interval, NULL);
}

static rmw_qos_profile_t qos(void) {
    rmw_qos_profile_t q = rmw_qos_profile_default;
    q.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    q.depth = DEPTH;
    q.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    q.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    return q;
}

struct endpoint_set {
    rmw_init_options_t options;
    rmw_context_t context;
    rmw_node_t* node;
};

// The next open_set()'s opts.segment.slot_bytes: 0, the default, unless a case sets it.
static uint32_t next_slot_bytes;

static void open_set(struct endpoint_set* set, const char* node_name) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    set->options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&set->options, allocator));
    set->options.enclave = rcutils_strdup("/", allocator);
    set->context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&set->options, &set->context));
    // The context's core options, between rmw_init() and the node that creates the core context from them.
    ((rmw_tickle_context_impl_t*)set->context.impl)->init_options.segment.slot_bytes = next_slot_bytes;
    set->node = rmw_create_node(&set->context, node_name, "/");
    assert(NULL != set->node);
}

static void close_set(struct endpoint_set* set) {
    assert(RMW_RET_OK == rmw_destroy_node(set->node));
    assert(RMW_RET_OK == rmw_shutdown(&set->context));
    assert(RMW_RET_OK == rmw_context_fini(&set->context));
    assert(RMW_RET_OK == rmw_init_options_fini(&set->options));
}

enum scenario { LOANED, TAKE, OFF };
static const char* const scenario_names[] = {"loaned", "take", "off"};
#define TOPIC "/large_loans"

// The talker: SAMPLES samples once matched, paced; then stays until the listener is done (RELIABLE repairs).
static int talker(int to_listener, int from_listener) {
    struct endpoint_set set;
    open_set(&set, "large_talker");
    rmw_publisher_options_t options = rmw_get_default_publisher_options();
    rmw_qos_profile_t q = qos();
    rmw_publisher_t* pub = rmw_create_publisher(set.node, &array1m_handle, TOPIC, &q, &options);
    assert(NULL != pub);
    bool matched = false;
    for (int i = 0; i < WAIT_POLLS && !matched; i++) {
        size_t count = 0;
        assert(RMW_RET_OK == rmw_publisher_count_matched_subscriptions(pub, &count));
        matched = count >= 1;
        if (!matched) {
            sleep_us(POLL_US);
        }
    }
    if (!matched) {
        return 2;
    }
    struct array1m* message = malloc(sizeof(*message));
    assert(NULL != message);
    for (uint64_t id = 1; id <= SAMPLES; id++) {
        fill(message, id);
        if (RMW_RET_OK != rmw_publish(pub, message, NULL)) {
            return 3;
        }
        sleep_us(PACE_US);
    }
    free(message);
    char done = 0;
    if (1 != write(to_listener, &done, 1) || 1 != read(from_listener, &done, 1)) {
        return 4;
    }
    assert(RMW_RET_OK == rmw_destroy_publisher(set.node, pub));
    close_set(&set);
    return 0;
}

// Takes one sample, by loan or into `into`; checks it whole and in order. False when there was none.
static bool take_one(enum scenario which, rmw_subscription_t* sub, struct array1m* into, uint64_t* next_id) {
    bool taken = false;
    const struct array1m* message = into;
    void* loan = NULL;
    if (LOANED == which) {
        assert(RMW_RET_OK == rmw_take_loaned_message(sub, &loan, &taken, NULL));
        message = loan;
    } else {
        assert(RMW_RET_OK == rmw_take(sub, into, &taken, NULL));
    }
    if (!taken) {
        return false;
    }
    if (message->id != *next_id || !intact(message)) {
        printf("FAIL: %s: sample id %llu (expected %llu) intact=%d\n", scenario_names[which],
               (unsigned long long)message->id, (unsigned long long)*next_id, intact(message) ? 1 : 0);
        assert(false);
    }
    assert(0 == ((uintptr_t)message & (_Alignof(struct array1m) - 1U))); // read through its own type
    (*next_id)++;
    if (NULL != loan) {
        assert(RMW_RET_OK == rmw_return_loaned_message_from_subscription(sub, loan));
    }
    return true;
}

static void run(enum scenario which) {
    if (OFF == which) {
        assert(0 == setenv("RMW_TICKLE_LOANS", "0", 1));
    } else {
        assert(0 == unsetenv("RMW_TICKLE_LOANS"));
    }
    int to_listener[2];
    int to_talker[2];
    assert(0 == pipe(to_listener) && 0 == pipe(to_talker));
    pid_t child = fork();
    assert(child >= 0);
    if (0 == child) {
        close(to_listener[0]);
        close(to_talker[1]);
        _exit(talker(to_listener[1], to_talker[0]));
    }
    close(to_listener[1]);
    close(to_talker[0]);
    assert(0 == fcntl(to_listener[0], F_SETFL, O_NONBLOCK));

    next_slot_bytes = SMALL_SLOT_BYTES; // read when the listener builds its ring, after the fork
    struct endpoint_set set;
    open_set(&set, "large_listener");
    rmw_tickle_context_impl_t* context_impl = ((rmw_tickle_node_t*)set.node->data)->context_impl;
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_qos_profile_t q = qos();
    rmw_subscription_t* sub = rmw_create_subscription(set.node, &array1m_handle, TOPIC, &q, &options);
    assert(NULL != sub);
    assert(sub->can_loan_messages == (OFF != which));

    struct array1m* into = malloc(sizeof(*into));
    assert(NULL != into);
    uint64_t next_id = 1;
    bool talker_done = false;
    for (int i = 0; i < WAIT_POLLS && next_id <= SAMPLES; i++) {
        char done = 0;
        talker_done = talker_done || 1 == read(to_listener[0], &done, 1);
        if (!take_one(which, sub, into, &next_id)) {
            sleep_us(POLL_US);
        }
    }
    free(into);
    char done = 1;
    assert(1 == write(to_talker[1], &done, 1));
    int status = 0;
    assert(child == waitpid(child, &status, 0));
    assert(WIFEXITED(status) && 0 == WEXITSTATUS(status));

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    tt_Context_lock(&context_impl->tickle_context);
    uint64_t retains = context_impl->tickle_context.lend.retains;
    uint64_t held = context_impl->tickle_context.lend.held;
    uint64_t reassembled = context_impl->tickle_context.large.reassembled;
    tt_Context_unlock(&context_impl->tickle_context);
    uint64_t received = next_id - 1;
    printf("%s: received %llu of %u; reassembled %llu; core retained %llu, still held %llu; loans in place %llu, "
           "copied %llu\n",
           scenario_names[which], (unsigned long long)received, SAMPLES, (unsigned long long)reassembled,
           (unsigned long long)retains, (unsigned long long)held, (unsigned long long)sub_impl->loans_in_place,
           (unsigned long long)sub_impl->loans_copied);
    assert(SAMPLES == received);
    assert(reassembled >= SAMPLES); // they came as large samples, or this tested nothing about them
    assert(0 == held);              // every loan and every retained sample went back
    switch (which) {
    case LOANED:
        assert(SAMPLES == sub_impl->loans_in_place && 0 == sub_impl->loans_copied);
        assert(retains >= SAMPLES);
        break;
    case TAKE:
        assert(retains >= SAMPLES);
        break;
    case OFF:
        assert(0 == retains && 0 == sub_impl->loans_in_place);
        break;
    }
    printf("%s: ok\n", scenario_names[which]);

    assert(RMW_RET_OK == rmw_destroy_subscription(set.node, sub));
    close_set(&set);
    next_slot_bytes = 0;
    close(to_listener[0]);
    close(to_talker[1]);
    assert(0 == unsetenv("RMW_TICKLE_LOANS"));
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0); // each case's line survives an assert in the next
    array1m_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    run(LOANED);
    run(TAKE);
    run(OFF);
    printf("test_large_loans: PASS\n");
    return 0;
}
