/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Loaned messages (docs/RMW.md, "Loaned messages"): the six rmw calls and can_loan_messages.
//
// In one process, with samples handed to the subscription's callback directly (core then refuses to retain them, so
// every loan is a decoded shell): which types lend (an Array1k-shaped type does, one whose callbacks say its wire
// bytes are not its message does not, and RMW_TICKLE_LOANS=0 turns lending off); every UNSUPPORTED and
// INVALID_ARGUMENT answer rmw.h asks for; a borrowed buffer is initialised when new, kept and bounded; a loaned take
// hands out the queued shell without copying, and the shell goes back to the pool on return.
//
// Across two processes (a forked talker, its own rmw context), with real delivery:
//   ring   Array1k through the shared-memory ring, published with borrow + publish_loaned, taken with take_loaned:
//          every sample arrives whole and in order, and - lending ring slots being off by default - no loan points
//          into the ring;
//   udp    the listener builds its ring with 512-byte slots, too small for an Array1k record, so the talker sends
//          them over UDP (segment_oversized_to_udp): loans are read in place, in a receive buffer;
//   pinned the ring again with RMW_TICKLE_LOAN_RING_SLOTS=1: loans are read in place, in the ring;
//   held   the listener holds loans on one topic and takes 3 laps of the ring's records on another, all of which must
//          arrive; the talker's ring refusals (segment_full_dropped) must stay 0;
//   held-pinned  the same with ring slots lent: the positive control. The held loans then pin ring slots, and the
//          talker's ring must refuse - if it did not, the check above could not see a held slot block a ring.
//   held-small   held again, with the listener's KEEP_ALL queue cut to a few samples
//   (RMW_TICKLE_READER_KEEP_ALL_BYTES),
//          so the flood is declined over and over: every sample must still arrive, and the declines must have
//          happened. A declined tail used to be asked for again only by a later Heartbeat or accepted sample, and
//          the talker, done, sent neither: 1600 of 1618 arrived, the 18 past its last piggybacked Heartbeat never
//          (2026-10-09; the held case failed the same way once in a gate run, 1141 of 1618). It catches the old
//          code in about half its runs - whether the tail is declined is timing - and core's
//          test_reliable_pubsub.c pins the mechanism deterministically.

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
#include <tickle/config.h>
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/error_handling.h"
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

#define ARRAY1K_BYTES 1024
#define RING_SAMPLES 300
#define HELD_LOANS 3
#define FLOOD_SAMPLES 1600 // three laps of the default 512-slot ring, and some
#define WARM_IN_SLOT 10    // records the talker must have written into the ring before the held samples go
#define WARM_MAX 2000
#define POLL_US 200L
#define WAIT_POLLS 50000 // 10 s at POLL_US
#define NS_PER_US 1000L
#define FILL_STEP 7U            // array[i] of sample `id` is (id * FILL_STEP + i) mod 256
#define TIME_PER_ID 1000U       // and its time is id * TIME_PER_ID
#define BORROW_MARK 99U         // the id a borrowed buffer is left holding
#define FIRST_PSN 11U           // the psns the one-process take check delivers with, one up per sample
#define UDP_SLOT_BYTES 512U     // a listener ring slot an Array1k record does not fit
#define RING_HEADER_SLACK 4096U // the segment header ahead of its slots, at most

// perf_test's Array1k: byte[1024], int64, uint64 - the int64 lands on 1024 on the wire and in memory alike.
struct array1k {
    uint8_t array[ARRAY1K_BYTES];
    int64_t time;
    uint64_t id;
};

// The direct codec of a type whose wire bytes are its struct: one memcpy each way, as the generated one is for it.
#define PLAIN_CODEC(name, type)                                                                                    \
    static int32_t name##_encode_size(const void* ros_message) {                                                   \
        (void)ros_message;                                                                                         \
        return (int32_t)sizeof(type);                                                                              \
    }                                                                                                              \
    static int32_t name##_encode(const void* ros_message, uint8_t* payload, uint32_t len) {                        \
        if (len < sizeof(type)) {                                                                                  \
            return -1;                                                                                             \
        }                                                                                                          \
        memcpy(payload, ros_message, sizeof(type));                                                                \
        return (int32_t)sizeof(type);                                                                              \
    }                                                                                                              \
    static int32_t name##_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) { \
        (void)is_native_endian;                                                                                    \
        if (len < sizeof(type)) {                                                                                  \
            return -1;                                                                                             \
        }                                                                                                          \
        memcpy(ros_message, payload, sizeof(type));                                                                \
        return (int32_t)sizeof(type);                                                                              \
    }

PLAIN_CODEC(array1k, struct array1k)

static int32_t refuse_encode_size(struct tt_Data* data) {
    (void)data;
    return -1;
}

#define PLAIN_CALLBACKS(name, type, type_name, inplace)                   \
    {                                                                     \
        .ros_type_name = (type_name),                                     \
        .tickle_struct_size = sizeof(type),                               \
        .ros_struct_size = sizeof(type),                                  \
        .tickle_encode_size = (tt_DATA_ENCODE_SIZE) & refuse_encode_size, \
        .tickle_max_encoded_size = sizeof(type),                          \
        .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,                 \
        .direct_encode_size = &name##_encode_size,                        \
        .direct_encode = &name##_encode,                                  \
        .direct_decode = &name##_decode,                                  \
        .inplace_bytes = (inplace),                                       \
        .ros_struct_align = _Alignof(type),                               \
    }

static rosidl_typesupport_tickle_c_message_callbacks_t array1k_callbacks =
    PLAIN_CALLBACKS(array1k, struct array1k, "test_loaned_messages/msg/Array1k", sizeof(struct array1k));
// The same bytes, but its callbacks do not claim them as its message: what a type that is not plain looks like here.
static rosidl_typesupport_tickle_c_message_callbacks_t opaque_callbacks =
    PLAIN_CALLBACKS(array1k, struct array1k, "test_loaned_messages/msg/Opaque", 0);

static rosidl_message_type_support_t array1k_handle = {.data = &array1k_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t opaque_handle = {.data = &opaque_callbacks,
                                                      .func = get_message_typesupport_handle_function};

static void fill_array1k(struct array1k* message, uint64_t id) {
    for (size_t i = 0; i < ARRAY1K_BYTES; i++) {
        message->array[i] = (uint8_t)((id * FILL_STEP) + i);
    }
    message->time = (int64_t)(id * TIME_PER_ID);
    message->id = id;
}

static bool array1k_intact(const struct array1k* message) {
    for (size_t i = 0; i < ARRAY1K_BYTES; i++) {
        if (message->array[i] != (uint8_t)((message->id * FILL_STEP) + i)) {
            return false;
        }
    }
    return message->time == (int64_t)(message->id * TIME_PER_ID);
}

static void sleep_poll(void) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = POLL_US * NS_PER_US};
    nanosleep(&interval, NULL);
}

static rmw_qos_profile_t qos_of(bool reliable, bool keep_all, size_t depth) {
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.history = keep_all ? RMW_QOS_POLICY_HISTORY_KEEP_ALL : RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = depth;
    qos.reliability = reliable ? RMW_QOS_POLICY_RELIABILITY_RELIABLE : RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    return qos;
}

struct endpoint_set {
    rmw_init_options_t options;
    rmw_context_t context;
    rmw_node_t* node;
};

static void open_set(struct endpoint_set* set, const char* node_name) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    set->options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&set->options, allocator));
    set->options.enclave = rcutils_strdup("/", allocator);
    set->context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&set->options, &set->context));
    set->node = rmw_create_node(&set->context, node_name, "/");
    assert(NULL != set->node);
}

static void close_set(struct endpoint_set* set) {
    assert(RMW_RET_OK == rmw_destroy_node(set->node));
    assert(RMW_RET_OK == rmw_shutdown(&set->context));
    assert(RMW_RET_OK == rmw_context_fini(&set->context));
    assert(RMW_RET_OK == rmw_init_options_fini(&set->options));
}

static rmw_publisher_t* make_publisher(rmw_node_t* node, const rosidl_message_type_support_t* type, const char* topic,
                                       rmw_qos_profile_t qos) {
    rmw_publisher_options_t options = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, type, topic, &qos, &options);
    assert(NULL != pub);
    return pub;
}

static rmw_subscription_t* make_subscription(rmw_node_t* node, const rosidl_message_type_support_t* type,
                                             const char* topic, rmw_qos_profile_t qos) {
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, type, topic, &qos, &options);
    assert(NULL != sub);
    return sub;
}

// ---------------------------------------------------------------------------------------------------------------
// One process

// Hands the subscription one message, as core's delivery would - but outside core, so core refuses to retain it.
static void deliver(rmw_subscription_t* sub, rmw_publisher_t* pub, const struct array1k* message, uint64_t psn) {
    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    rmw_tickle_outgoing_message_t outgoing = {.publication_sequence_number = psn,
                                              .callbacks = &array1k_callbacks,
                                              .ros_message = message,
                                              .tickle = NULL};
    uint8_t wire[sizeof(struct array1k) + RMW_TICKLE_PSN_BYTES];
    int32_t size = pub_impl->topic.data_encode((struct tt_Data*)&outgoing, wire, (uint32_t)sizeof(wire));
    assert(size > 0);
    sub_impl->tickle_subscriber.callback(&sub_impl->tickle_subscriber, 0, (uint16_t)psn,
                                         sub_impl->topic.data_decode_inplace(wire, (uint32_t)size, true));
}

static void check_unsupported(rmw_node_t* node) {
    rmw_publisher_t* pub = make_publisher(node, &opaque_handle, "/loan_opaque", qos_of(false, false, 4));
    rmw_subscription_t* sub = make_subscription(node, &opaque_handle, "/loan_opaque", qos_of(false, false, 4));
    assert(!pub->can_loan_messages && !sub->can_loan_messages);
    void* message = NULL;
    bool taken = true;
    rmw_message_info_t info = rmw_get_zero_initialized_message_info();
    assert(RMW_RET_UNSUPPORTED == rmw_borrow_loaned_message(pub, &opaque_handle, &message) && NULL == message);
    rmw_reset_error();
    struct array1k scratch;
    assert(RMW_RET_UNSUPPORTED == rmw_return_loaned_message_from_publisher(pub, &scratch));
    rmw_reset_error();
    assert(RMW_RET_UNSUPPORTED == rmw_publish_loaned_message(pub, &scratch, NULL));
    rmw_reset_error();
    assert(RMW_RET_UNSUPPORTED == rmw_take_loaned_message(sub, &message, &taken, NULL));
    rmw_reset_error();
    assert(RMW_RET_UNSUPPORTED == rmw_take_loaned_message_with_info(sub, &message, &taken, &info, NULL));
    rmw_reset_error();
    assert(RMW_RET_UNSUPPORTED == rmw_return_loaned_message_from_subscription(sub, &scratch));
    rmw_reset_error();
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));

    // RMW_TICKLE_LOANS=0: a type that could lend does not.
    assert(0 == setenv("RMW_TICKLE_LOANS", "0", 1));
    pub = make_publisher(node, &array1k_handle, "/loan_off", qos_of(false, false, 4));
    sub = make_subscription(node, &array1k_handle, "/loan_off", qos_of(false, false, 4));
    assert(!pub->can_loan_messages && !sub->can_loan_messages);
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(0 == unsetenv("RMW_TICKLE_LOANS"));
    printf("unsupported: an opaque type and RMW_TICKLE_LOANS=0 do not lend; all six calls UNSUPPORTED\n");
}

static void check_borrow(rmw_node_t* node) {
    rmw_publisher_t* pub = make_publisher(node, &array1k_handle, "/loan_borrow", qos_of(false, false, 4));
    assert(pub->can_loan_messages);

    void* message = (void*)&message;
    assert(RMW_RET_INVALID_ARGUMENT == rmw_borrow_loaned_message(pub, &array1k_handle, &message)); // *msg not NULL
    rmw_reset_error();
    message = NULL;
    assert(RMW_RET_INVALID_ARGUMENT == rmw_borrow_loaned_message(pub, &opaque_handle, &message)); // another type
    rmw_reset_error();
    assert(RMW_RET_INVALID_ARGUMENT == rmw_borrow_loaned_message(pub, NULL, &message));
    rmw_reset_error();

    assert(RMW_RET_OK == rmw_borrow_loaned_message(pub, &array1k_handle, &message) && NULL != message);
    assert(0 == ((uintptr_t)message & (_Alignof(struct array1k) - 1U)));
    struct array1k* first = message;
    static const struct array1k zero;
    assert(0 == memcmp(first, &zero, sizeof(zero))); // initialised, as a fresh message is
    fill_array1k(first, BORROW_MARK);

    struct array1k foreign;
    assert(RMW_RET_INVALID_ARGUMENT == rmw_return_loaned_message_from_publisher(pub, &foreign));
    rmw_reset_error();
    assert(RMW_RET_INVALID_ARGUMENT == rmw_publish_loaned_message(pub, &foreign, NULL));
    rmw_reset_error();
    assert(RMW_RET_OK == rmw_return_loaned_message_from_publisher(pub, first));
    assert(RMW_RET_INVALID_ARGUMENT == rmw_return_loaned_message_from_publisher(pub, first)); // twice
    rmw_reset_error();

    // The buffer is kept and lent again, as the last loan left it (a loanable type needs no constructing).
    message = NULL;
    assert(RMW_RET_OK == rmw_borrow_loaned_message(pub, &array1k_handle, &message));
    assert(message == first);
    assert(BORROW_MARK == first->id);
    // Publishing it gives it back as well (nobody is listening; the publish itself succeeds).
    fill_array1k(first, 5);
    // loans_published (the treatment witness of ab_loans.sh) counts loans published and nothing else: not a return,
    // not a refused publish of a buffer that is not a loan.
    const rmw_tickle_publisher_t* counted = (const rmw_tickle_publisher_t*)pub->data;
    assert(0 == counted->loans_published);
    assert(RMW_RET_OK == rmw_publish_loaned_message(pub, first, NULL));
    assert(1 == counted->loans_published);
    assert(RMW_RET_INVALID_ARGUMENT == rmw_publish_loaned_message(pub, first, NULL)); // published already
    rmw_reset_error();
    assert(1 == counted->loans_published);

    // Bounded: RMW_TICKLE_PUBLISHER_LOANS_MAX out at once, then BAD_ALLOC, then fine again once one comes back.
    void* out[RMW_TICKLE_PUBLISHER_LOANS_MAX];
    for (size_t i = 0; i < RMW_TICKLE_PUBLISHER_LOANS_MAX; i++) {
        out[i] = NULL;
        assert(RMW_RET_OK == rmw_borrow_loaned_message(pub, &array1k_handle, &out[i]));
        for (size_t j = 0; j < i; j++) {
            assert(out[j] != out[i]);
        }
    }
    message = NULL;
    assert(RMW_RET_BAD_ALLOC == rmw_borrow_loaned_message(pub, &array1k_handle, &message));
    rmw_reset_error();
    assert(RMW_RET_OK == rmw_return_loaned_message_from_publisher(pub, out[7]));
    assert(RMW_RET_OK == rmw_borrow_loaned_message(pub, &array1k_handle, &message) && message == out[7]);
    for (size_t i = 0; i < RMW_TICKLE_PUBLISHER_LOANS_MAX; i++) {
        assert(RMW_RET_OK == rmw_return_loaned_message_from_publisher(pub, out[i]));
    }
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    printf("borrow: contract answers, a kept buffer lent again, %u at once then BAD_ALLOC\n",
           (unsigned)RMW_TICKLE_PUBLISHER_LOANS_MAX);
}

static void check_take(rmw_node_t* node) {
    rmw_publisher_t* pub = make_publisher(node, &array1k_handle, "/loan_take", qos_of(false, false, 4));
    rmw_subscription_t* sub = make_subscription(node, &array1k_handle, "/loan_take", qos_of(false, false, 4));
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    assert(sub->can_loan_messages);

    void* message = NULL;
    bool taken = true;
    rmw_message_info_t info = rmw_get_zero_initialized_message_info();
    assert(RMW_RET_OK == rmw_take_loaned_message(sub, &message, &taken, NULL) && !taken && NULL == message);
    assert(RMW_RET_INVALID_ARGUMENT == rmw_take_loaned_message_with_info(sub, &message, &taken, NULL, NULL));
    rmw_reset_error();

    struct array1k sample;
    fill_array1k(&sample, 1);
    deliver(sub, pub, &sample, FIRST_PSN);
    fill_array1k(&sample, 2);
    deliver(sub, pub, &sample, FIRST_PSN + 1);

    message = (void*)&message;
    assert(RMW_RET_INVALID_ARGUMENT == rmw_take_loaned_message(sub, &message, &taken, NULL)); // *msg not NULL
    rmw_reset_error();
    message = NULL;
    assert(RMW_RET_OK == rmw_take_loaned_message_with_info(sub, &message, &taken, &info, NULL) && taken);
    const struct array1k* first = message;
    assert(1 == first->id && array1k_intact(first) && FIRST_PSN == info.publication_sequence_number);
    // Delivered outside core, so core would not retain it: a decoded shell is lent, taken from the queue as it is.
    assert(1 == sub_impl->loans_copied && 0 == sub_impl->loans_in_place && 1 == sub_impl->loans_out_count);
    size_t pooled = sub_impl->shell_pool_count;

    void* second = NULL;
    assert(RMW_RET_OK == rmw_take_loaned_message(sub, &second, &taken, NULL) && taken);
    assert(2 == ((const struct array1k*)second)->id && second != message);

    struct array1k foreign;
    assert(RMW_RET_INVALID_ARGUMENT == rmw_return_loaned_message_from_subscription(sub, &foreign));
    rmw_reset_error();
    assert(RMW_RET_OK == rmw_return_loaned_message_from_subscription(sub, message));
    assert(pooled + 1 == sub_impl->shell_pool_count); // the shell is the pool's again
    assert(RMW_RET_INVALID_ARGUMENT == rmw_return_loaned_message_from_subscription(sub, message)); // twice
    rmw_reset_error();
    assert(RMW_RET_OK == rmw_return_loaned_message_from_subscription(sub, second));
    assert(0 == sub_impl->loans_out_count);

    // An ordinary take still works on a loaning subscription.
    fill_array1k(&sample, 3);
    deliver(sub, pub, &sample, FIRST_PSN + 2);
    struct array1k copy;
    assert(RMW_RET_OK == rmw_take(sub, &copy, &taken, NULL) && taken && 3 == copy.id && array1k_intact(&copy));

    // A loan still out when the subscription goes is given back, not leaked (the destroy warns).
    fill_array1k(&sample, 4);
    deliver(sub, pub, &sample, FIRST_PSN + 3);
    message = NULL;
    assert(RMW_RET_OK == rmw_take_loaned_message(sub, &message, &taken, NULL) && taken);
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    printf("take: contract answers, a queued shell lent and returned to the pool, ordinary take unchanged\n");
}

// ---------------------------------------------------------------------------------------------------------------
// Two processes

enum scenario { RING, UDP, PINNED, HELD, HELD_PINNED, HELD_SMALL };
static const char* const scenario_names[] = {"ring", "udp", "pinned", "held", "held-pinned", "held-small"};
#define SMALL_QUEUE_BYTES "8000" // the held-small listener's KEEP_ALL budget: a handful of Array1k samples

static bool is_held(enum scenario which) {
    return HELD == which || HELD_PINNED == which || HELD_SMALL == which;
}

#define TOPIC_DATA "/loan_data"
#define TOPIC_HELD "/loan_held"

// What the talker says once it has published everything it could: how many data samples went (ids 1..published),
// how many publishes failed (it stops at the first), and how often its ring refused a record.
struct talker_report {
    uint64_t segment_full_dropped;
    uint64_t published;
    uint64_t failed;
};

static bool wait_matched_subscriptions(rmw_publisher_t* pub, size_t want) {
    for (int i = 0; i < WAIT_POLLS; i++) {
        size_t count = 0;
        assert(RMW_RET_OK == rmw_publisher_count_matched_subscriptions(pub, &count));
        if (count >= want) {
            return true;
        }
        sleep_poll();
    }
    return false;
}

static bool wait_matched_publishers(rmw_subscription_t* sub, size_t want) {
    for (int i = 0; i < WAIT_POLLS; i++) {
        size_t count = 0;
        assert(RMW_RET_OK == rmw_subscription_count_matched_publishers(sub, &count));
        if (count >= want) {
            return true;
        }
        sleep_poll();
    }
    return false;
}

static rmw_ret_t publish_array1k_loaned(rmw_publisher_t* pub, uint64_t id) {
    void* message = NULL;
    assert(RMW_RET_OK == rmw_borrow_loaned_message(pub, &array1k_handle, &message));
    fill_array1k(message, id);
    rmw_ret_t ret = rmw_publish_loaned_message(pub, message, NULL);
    rmw_reset_error();
    return ret;
}

// The held cases' talking: traffic first, until the talker writes into the listener's ring (the samples before that go
// over UDP, unattached), so the held samples go through the ring; then the held samples; then the flood, once the
// listener says it holds them. 0, or the talker's exit code.
static int talk_held(rmw_tickle_context_impl_t* context_impl, rmw_publisher_t* data, rmw_publisher_t* held,
                     int from_listener, struct talker_report* report) {
    uint64_t warm = 0;
    bool attached = false;
    while (!attached && warm < WARM_MAX) {
        warm++;
        assert(RMW_RET_OK == publish_array1k_loaned(data, warm));
        sleep_poll();
        tt_Context_lock(&context_impl->tickle_context);
        attached = context_impl->tickle_context.segment_encoded_in_slot >= WARM_IN_SLOT;
        tt_Context_unlock(&context_impl->tickle_context);
    }
    if (!attached) {
        return 5;
    }
    for (uint64_t id = 1; id <= HELD_LOANS; id++) {
        assert(RMW_RET_OK == publish_array1k_loaned(held, id));
    }
    report->published = warm;
    char signal = 0;
    if (1 != read(from_listener, &signal, 1)) {
        return 3;
    }
    for (uint64_t id = warm + 1; id <= warm + FLOOD_SAMPLES && 0 == report->failed; id++) {
        if (RMW_RET_OK == publish_array1k_loaned(data, id)) {
            report->published = id;
        } else {
            report->failed++;
        }
    }
    return 0;
}

// The talker: publishes with loans, and reports its own ring refusals. Exits nonzero on any failure.
static int talker(enum scenario which, int to_listener, int from_listener) {
    struct endpoint_set set;
    open_set(&set, "loan_talker");
    rmw_tickle_context_impl_t* context_impl = ((rmw_tickle_node_t*)set.node->data)->context_impl;
    struct talker_report report = {0, 0, 0};
    // A KEEP_ALL publish blocked on a stopped ring gives up after this long (the control case is the one that blocks).
    assert(0 == setenv("RMW_TICKLE_MAX_BLOCKING_MS", "300", 1));
    rmw_publisher_t* data = make_publisher(set.node, &array1k_handle, TOPIC_DATA, qos_of(true, true, 0));
    rmw_publisher_t* held = NULL;
    if (is_held(which)) {
        held = make_publisher(set.node, &array1k_handle, TOPIC_HELD, qos_of(true, true, 0));
        if (!wait_matched_subscriptions(held, 1)) {
            return 2;
        }
    }
    if (!wait_matched_subscriptions(data, 1)) {
        return 2;
    }
    if (NULL != held) {
        int code = talk_held(context_impl, data, held, from_listener, &report);
        if (0 != code) {
            return code;
        }
    } else {
        // Paced, so the listener keeps up: a queue deeper than RMW_TICKLE_LOAN_RETAIN_PER_SUBSCRIPTION holds the rest
        // decoded, and how much is read in place is what these cases measure.
        for (uint64_t id = 1; id <= RING_SAMPLES; id++) {
            assert(RMW_RET_OK == publish_array1k_loaned(data, id));
            sleep_poll();
        }
        report.published = RING_SAMPLES;
    }
    tt_Context_lock(&context_impl->tickle_context);
    report.segment_full_dropped = context_impl->tickle_context.segment_full_dropped;
    tt_Context_unlock(&context_impl->tickle_context);
    if (sizeof(report) != (size_t)write(to_listener, &report, sizeof(report))) {
        return 4;
    }
    // Stay until the listener is done, so RELIABLE repairs can still be answered.
    char done = 0;
    if (1 != read(from_listener, &done, 1)) {
        return 3;
    }
    assert(RMW_RET_OK == rmw_destroy_publisher(set.node, data));
    if (NULL != held) {
        assert(RMW_RET_OK == rmw_destroy_publisher(set.node, held));
    }
    close_set(&set);
    return 0;
}

// Where a loan's bytes are: the listener's own ring, one of its receive buffers, or elsewhere (a decoded shell).
enum where { IN_RING, IN_RECEIVE_BUFFER, ELSEWHERE };

static enum where where_is(const rmw_tickle_context_impl_t* context_impl, const void* message) {
    const uint8_t* bytes = message;
    const uint8_t* ring = (const uint8_t*)context_impl->tickle_context.own_segment;
    if (NULL != ring && bytes >= ring && bytes < ring + (size_t)tt_SEGMENT_BYTES + RING_HEADER_SLACK) {
        return IN_RING;
    }
    const uint8_t* inline_buffer = context_impl->tickle_context.rx_buffer;
    if (bytes >= inline_buffer && bytes < inline_buffer + sizeof(context_impl->tickle_context.rx_buffer)) {
        return IN_RECEIVE_BUFFER;
    }
    const uint8_t* pool = (const uint8_t*)context_impl->loan_rx_pool;
    if (NULL != pool && bytes >= pool &&
        bytes < pool + ((size_t)RMW_TICKLE_LOAN_RX_POOL_BUFFERS * tt_RX_POOL_BUFFER_BYTES)) {
        return IN_RECEIVE_BUFFER;
    }
    return ELSEWHERE;
}

struct tally {
    uint64_t received;
    uint64_t next_id;
    uint64_t where[3];
};

// Takes one loaned message off `sub` if there is one, checks it whole and in order, and returns it.
static bool take_checked(rmw_subscription_t* sub, const rmw_tickle_context_impl_t* context_impl, struct tally* tally) {
    void* message = NULL;
    bool taken = false;
    rmw_message_info_t info = rmw_get_zero_initialized_message_info();
    assert(RMW_RET_OK == rmw_take_loaned_message_with_info(sub, &message, &taken, &info, NULL));
    if (!taken) {
        return false;
    }
    uint64_t id = ((const struct array1k*)message)->id;
    bool intact = array1k_intact(message);
    if (!intact || id != tally->next_id) {
        printf("FAIL: sample id %llu (expected %llu) intact=%d\n", (unsigned long long)id,
               (unsigned long long)tally->next_id, intact ? 1 : 0);
        assert(false);
    }
    // A message is read through its own type: lent where its int64 would be misaligned, it would not be one.
    assert(0 == ((uintptr_t)message & (_Alignof(struct array1k) - 1U)));
    tally->next_id++;
    tally->received++;
    tally->where[where_is(context_impl, message)]++;
    assert(RMW_RET_OK == rmw_return_loaned_message_from_subscription(sub, message));
    return true;
}

// The held topic's samples, taken and kept (the data topic drained meanwhile); their addresses go to held_where.
static void take_held_loans(rmw_subscription_t* data, rmw_subscription_t* held,
                            const rmw_tickle_context_impl_t* context_impl, struct tally* tally, uint64_t* held_where,
                            void** held_loans) {
    size_t got = 0;
    for (int i = 0; i < WAIT_POLLS && got < HELD_LOANS; i++) {
        while (take_checked(data, context_impl, tally)) {
        }
        bool taken = false;
        void* message = NULL;
        assert(RMW_RET_OK == rmw_take_loaned_message(held, &message, &taken, NULL));
        if (taken) {
            assert(array1k_intact(message) && got + 1 == ((const struct array1k*)message)->id);
            held_where[where_is(context_impl, message)]++;
            held_loans[got++] = message;
        } else {
            sleep_poll();
        }
    }
    assert(HELD_LOANS == got);
}

// Takes the data topic until the talker has reported and everything it published is in, or 2 s after its report.
static struct talker_report receive_until_reported(rmw_subscription_t* data,
                                                   const rmw_tickle_context_impl_t* context_impl, int from_talker,
                                                   struct tally* tally) {
    struct talker_report report = {0, 0, 0};
    bool reported = false;
    int after_report = 0;
    for (int i = 0; i < WAIT_POLLS * 3 && after_report < WAIT_POLLS / 5; i++) {
        if (!reported) {
            // The pipe is non-blocking: nothing there yet is EAGAIN, not a wait.
            reported = sizeof(report) == (size_t)read(from_talker, &report, sizeof(report));
        }
        if (reported && tally->received >= report.published) {
            break;
        }
        after_report += reported ? 1 : 0;
        if (!take_checked(data, context_impl, tally)) {
            sleep_poll();
        }
    }
    assert(reported);
    return report;
}

// What each case requires. The line printed after the checks is what the mutant sweep reads.
static void check_case(enum scenario which, const struct tally* tally, const uint64_t* held_where,
                       const struct talker_report* report, uint64_t want, uint64_t declines) {
    switch (which) {
    case RING:
        assert(want == tally->received && want == report->published);
        assert(0 == tally->where[IN_RING]);          // ring slots are not lent by default
        assert(tally->where[ELSEWHERE] > want / 2U); // the ring carried most of them
        break;
    case UDP:
        assert(want == tally->received && want == report->published);
        assert(0 == tally->where[IN_RING]);
        // Read where they arrived - but only those that landed in a pool buffer: core's inline rx_buffer sits 4 bytes
        // off 8 in this build, so an Array1k there is not aligned for its int64 and is copied (docs/RMW.md).
        assert(tally->where[IN_RECEIVE_BUFFER] > 0);
        break;
    case PINNED:
        assert(want == tally->received && want == report->published);
        assert(tally->where[IN_RING] > want / 2U);
        break;
    case HELD:
        // every flood sample went, and arrived, while the loans were held
        assert(report->published > FLOOD_SAMPLES && report->published == tally->received && 0 == report->failed);
        assert(0 == held_where[IN_RING]);          // so nothing pinned the ring...
        assert(0 == report->segment_full_dropped); // ...and it never refused the talker
        break;
    case HELD_SMALL:
        // as held - and the queue did decline, or this case tested nothing the held case does not
        assert(report->published > FLOOD_SAMPLES && report->published == tally->received && 0 == report->failed);
        assert(declines > 0);
        break;
    case HELD_PINNED:
        // The control: with the held loans in ring slots, the ring must have refused the talker - which the case
        // above requires never to happen. Its KEEP_ALL publishes then block on samples nobody can acknowledge, and
        // give up (RMW_TICKLE_MAX_BLOCKING_MS), which the case above also rules out.
        assert(held_where[IN_RING] > 0);
        assert(report->segment_full_dropped > 0);
        assert(0 != report->failed);
        break;
    }
    printf("%s: ok\n", scenario_names[which]);
}

static void run_two_process(enum scenario which) {
    bool pinned = PINNED == which || HELD_PINNED == which;
    if (pinned) {
        assert(0 == setenv("RMW_TICKLE_LOAN_RING_SLOTS", "1", 1));
    } else {
        assert(0 == unsetenv("RMW_TICKLE_LOAN_RING_SLOTS"));
    }
    int to_listener[2];
    int to_talker[2];
    assert(0 == pipe(to_listener) && 0 == pipe(to_talker));
    pid_t child = fork();
    assert(child >= 0);
    if (0 == child) {
        close(to_listener[0]);
        close(to_talker[1]);
        _exit(talker(which, to_listener[1], to_talker[0]));
    }
    close(to_listener[1]);
    close(to_talker[0]);
    assert(0 == fcntl(to_listener[0], F_SETFL, O_NONBLOCK));

    // UDP: a ring whose slots an Array1k record does not fit, so the talker sends those over UDP. Read when the
    // listener builds its ring, which is after this; the talker, forked already, keeps the default.
    _tt_CONFIG.segment_slot_bytes = UDP == which ? UDP_SLOT_BYTES : 0U;
    struct endpoint_set set;
    open_set(&set, "loan_listener");
    rmw_tickle_context_impl_t* context_impl = ((rmw_tickle_node_t*)set.node->data)->context_impl;
    bool held_case = is_held(which);
    if (HELD_SMALL == which) {
        assert(0 == setenv("RMW_TICKLE_READER_KEEP_ALL_BYTES", SMALL_QUEUE_BYTES, 1)); // read at subscription creation
    }
    rmw_subscription_t* data = make_subscription(set.node, &array1k_handle, TOPIC_DATA, qos_of(true, true, 0));
    assert(0 == unsetenv("RMW_TICKLE_READER_KEEP_ALL_BYTES"));
    rmw_subscription_t* held =
        held_case ? make_subscription(set.node, &array1k_handle, TOPIC_HELD, qos_of(true, true, 0)) : NULL;
    assert(data->can_loan_messages);
    assert(wait_matched_publishers(data, 1));

    struct tally tally = {.received = 0, .next_id = 1, .where = {0, 0, 0}};
    uint64_t want = RING_SAMPLES; // the held cases: the talker's report says how many it sent before the flood
    uint64_t held_where[3] = {0, 0, 0};
    void* held_loans[HELD_LOANS];
    if (held_case) {
        take_held_loans(data, held, context_impl, &tally, held_where, held_loans);
        char signal = 1;
        assert(1 == write(to_talker[1], &signal, 1));
    }
    struct talker_report report = receive_until_reported(data, context_impl, to_listener[0], &tally);
    if (held_case) {
        want = report.published; // the warm-up's length is the talker's to decide
    }
    char done = 1;
    assert(1 == write(to_talker[1], &done, 1));
    int status = 0;
    assert(child == waitpid(child, &status, 0));
    assert(WIFEXITED(status) && 0 == WEXITSTATUS(status));

    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)data->data;
    printf("%s: received %llu of %llu published (%llu meant, %llu publishes failed); loans in ring %llu, in a receive "
           "buffer %llu, decoded %llu (in place %llu, copied %llu); held loans in ring %llu, in a buffer %llu, decoded "
           "%llu; talker ring refusals %llu\n",
           scenario_names[which], (unsigned long long)tally.received, (unsigned long long)report.published,
           (unsigned long long)want, (unsigned long long)report.failed, (unsigned long long)tally.where[IN_RING],
           (unsigned long long)tally.where[IN_RECEIVE_BUFFER], (unsigned long long)tally.where[ELSEWHERE],
           (unsigned long long)sub_impl->loans_in_place, (unsigned long long)sub_impl->loans_copied,
           (unsigned long long)held_where[IN_RING], (unsigned long long)held_where[IN_RECEIVE_BUFFER],
           (unsigned long long)held_where[ELSEWHERE], (unsigned long long)report.segment_full_dropped);
    // Where the loans came from is the point of each case; `where` is decided by address, independently of the
    // counters the rmw keeps, and the two must agree.
    assert(tally.where[IN_RING] + tally.where[IN_RECEIVE_BUFFER] == sub_impl->loans_in_place);
    assert(tally.where[ELSEWHERE] == sub_impl->loans_copied);
    tt_Context_lock(&context_impl->tickle_context);
    uint64_t declines = sub_impl->tickle_subscriber.accept_declines;
    tt_Context_unlock(&context_impl->tickle_context);
    printf("%s: the listener's queue declined %llu samples\n", scenario_names[which], (unsigned long long)declines);
    check_case(which, &tally, held_where, &report, want, declines);
    if (held_case) {
        for (size_t i = 0; i < HELD_LOANS; i++) {
            assert(RMW_RET_OK == rmw_return_loaned_message_from_subscription(held, held_loans[i]));
        }
        assert(RMW_RET_OK == rmw_destroy_subscription(set.node, held));
    }
    assert(RMW_RET_OK == rmw_destroy_subscription(set.node, data));
    close_set(&set);
    _tt_CONFIG.segment_slot_bytes = 0;
    close(to_listener[0]);
    close(to_talker[1]);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0); // each case's line survives an assert in the next
    array1k_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    opaque_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    assert(0 == unsetenv("RMW_TICKLE_LOANS"));

    struct endpoint_set set;
    open_set(&set, "test_loaned_messages");
    check_unsupported(set.node);
    check_borrow(set.node);
    check_take(set.node);
    close_set(&set);

    run_two_process(RING);
    run_two_process(UDP);
    run_two_process(PINNED);
    run_two_process(HELD);
    run_two_process(HELD_PINNED);
    run_two_process(HELD_SMALL);
    printf("test_loaned_messages: PASS\n");
    return 0;
}
