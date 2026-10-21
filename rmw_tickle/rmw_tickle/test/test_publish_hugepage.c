/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// RMW_TICKLE_PUBLISH_HUGEPAGE (rmw_large.c): a buffer a large sample is published from is huge-page aligned and
// advised MADV_HUGEPAGE when it is at least one huge page; a buffer a sample is put back together in is not.
//
// Two processes (a forked talker, its own rmw context), a 3 MiB type, RELIABLE KEEP_LAST, sent as fragments over UDP
// (RMW_TICKLE_LARGE_REGION=0 and a listener ring too small for a fragment record, so the talker's buffers are the
// context's own, not a shared region's chunks):
//   on   the switch unset (the default): the talker allocated a publish buffer and advised it (unless this kernel has
//        no THP, when madvise() fails and the advice is silently not taken); the listener allocated reassembly
//        buffers, none for a publish and none advised.
//   off  RMW_TICKLE_PUBLISH_HUGEPAGE=0, the control: the same publish buffer allocated and never advised - so what the
//        "on" case reads is the switch, not something every publish shows.
// Each side's realized AnonHugePages is printed, not asserted: THP "never" honours no advice.

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

#define ARRAY_BYTES ((size_t)3 * 1024 * 1024) // above one 2 MiB huge page, below the 8 MiB default limit
#define SAMPLES 5U
#define DEPTH 5U
#define PACE_US 100000L // between publishes: a 3 MiB sample is ~2,200 datagrams
#define POLL_US 200L
#define WAIT_POLLS 75000 // 15 s at POLL_US
#define NS_PER_US 1000L
#define SMALL_SLOT_BYTES 512U // a listener ring slot a fragment record does not fit: the talker sends over UDP
#define TALKER_NOT_ADVISED 5
#define TALKER_NO_PUBLISH_BUFFER 6

struct array3m {
    uint8_t array[ARRAY_BYTES];
    uint64_t id;
};

static int32_t array3m_encode_size(const void* ros_message) {
    (void)ros_message;
    return (int32_t)sizeof(struct array3m);
}

static int32_t array3m_encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    if (len < sizeof(struct array3m)) {
        return -1;
    }
    memcpy(payload, ros_message, sizeof(struct array3m));
    return (int32_t)sizeof(struct array3m);
}

static int32_t array3m_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct array3m)) {
        return -1;
    }
    memcpy(ros_message, payload, sizeof(struct array3m));
    return (int32_t)sizeof(struct array3m);
}

static int32_t refuse_encode_size(struct tt_Data* data) {
    (void)data;
    return -1;
}

static rosidl_typesupport_tickle_c_message_callbacks_t array3m_callbacks = {
    .ros_type_name = "test_publish_hugepage/msg/Array3m",
    .tickle_struct_size = sizeof(struct array3m),
    .ros_struct_size = sizeof(struct array3m),
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&refuse_encode_size,
    .tickle_max_encoded_size = sizeof(struct array3m),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
    .direct_encode_size = &array3m_encode_size,
    .direct_encode = &array3m_encode,
    .direct_decode = &array3m_decode,
    .inplace_bytes = sizeof(struct array3m),
    .ros_struct_align = _Alignof(struct array3m),
};

static rosidl_message_type_support_t array3m_handle = {.data = &array3m_callbacks,
                                                       .func = get_message_typesupport_handle_function};

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

// Whether this kernel offers transparent huge pages at all: without them madvise(MADV_HUGEPAGE) fails, and the
// fallback is to publish from ordinary pages without a word.
static bool thp_available(void) {
    return 0 == access("/sys/kernel/mm/transparent_hugepage/enabled", R_OK);
}

static rmw_tickle_context_impl_t* context_of(const struct endpoint_set* set) {
    return ((rmw_tickle_node_t*)set->node->data)->context_impl;
}

enum scenario { ON, OFF };
static const char* const scenario_names[] = {"on", "off"};
#define TOPIC "/publish_hugepage"

// The talker: SAMPLES samples once matched, paced; then its pool's counts decide its exit code.
static int talker(enum scenario which, int to_listener, int from_listener) {
    struct endpoint_set set;
    open_set(&set, "hugepage_talker");
    rmw_publisher_options_t options = rmw_get_default_publisher_options();
    rmw_qos_profile_t q = qos();
    rmw_publisher_t* pub = rmw_create_publisher(set.node, &array3m_handle, TOPIC, &q, &options);
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
    struct array3m* message = calloc(1, sizeof(*message));
    assert(NULL != message);
    for (uint64_t id = 1; id <= SAMPLES; id++) {
        memset(message->array, (int)id, sizeof(message->array));
        message->id = id;
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
    struct rmw_tickle_large_stats stats;
    rmw_tickle_large_stats(context_of(&set), &stats);
    printf("%s talker: publish_hugepage=%d allocated=%u publish_allocated=%u advised=%u\n", scenario_names[which],
           stats.publish_hugepage ? 1 : 0, (unsigned)stats.allocated, (unsigned)stats.publish_allocated,
           (unsigned)stats.advised);
    assert(RMW_RET_OK == rmw_destroy_publisher(set.node, pub));
    close_set(&set); // prints the pool's line, with the realized AnonHugePages
    if (stats.publish_allocated < 1 || stats.publish_hugepage != (ON == which)) {
        return TALKER_NO_PUBLISH_BUFFER;
    }
    bool want_advised = ON == which && thp_available();
    if ((stats.advised >= 1) != want_advised || stats.advised > stats.publish_allocated) {
        return TALKER_NOT_ADVISED;
    }
    return 0;
}

static void run(enum scenario which) {
    if (OFF == which) {
        assert(0 == setenv("RMW_TICKLE_PUBLISH_HUGEPAGE", "0", 1));
    } else {
        assert(0 == unsetenv("RMW_TICKLE_PUBLISH_HUGEPAGE"));
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

    next_slot_bytes = SMALL_SLOT_BYTES; // read when the listener builds its ring, after the fork
    struct endpoint_set set;
    open_set(&set, "hugepage_listener");
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_qos_profile_t q = qos();
    rmw_subscription_t* sub = rmw_create_subscription(set.node, &array3m_handle, TOPIC, &q, &options);
    assert(NULL != sub);

    struct array3m* into = malloc(sizeof(*into));
    assert(NULL != into);
    uint64_t received = 0;
    bool talker_done = false;
    for (int i = 0; i < WAIT_POLLS && !talker_done; i++) {
        char done = 0;
        talker_done = 1 == read(to_listener[0], &done, 1);
        bool taken = false;
        assert(RMW_RET_OK == rmw_take(sub, into, &taken, NULL));
        if (taken) {
            assert(into->array[0] == (uint8_t)into->id && into->array[ARRAY_BYTES - 1U] == (uint8_t)into->id);
            received++;
        } else {
            sleep_us(POLL_US);
        }
    }
    free(into);
    struct rmw_tickle_large_stats stats;
    rmw_tickle_large_stats(context_of(&set), &stats);
    char done = 1;
    assert(1 == write(to_talker[1], &done, 1));
    int status = 0;
    assert(child == waitpid(child, &status, 0));
    printf("%s listener: received %llu of %u; publish_hugepage=%d allocated=%u publish_allocated=%u advised=%u; "
           "talker exit %d\n",
           scenario_names[which], (unsigned long long)received, SAMPLES, stats.publish_hugepage ? 1 : 0,
           (unsigned)stats.allocated, (unsigned)stats.publish_allocated, (unsigned)stats.advised,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    assert(WIFEXITED(status) && 0 == WEXITSTATUS(status));
    assert(received >= 1);                // something was put back together, or the listener's counts say nothing
    assert(stats.allocated >= 1);         // in a buffer of its own context's pool
    assert(0 == stats.publish_allocated); // never as a publish's
    assert(0 == stats.advised);           // and never advised: reassembly buffers stay ordinary pages
    printf("%s: ok\n", scenario_names[which]);

    assert(RMW_RET_OK == rmw_destroy_subscription(set.node, sub));
    close_set(&set);
    next_slot_bytes = 0;
    close(to_listener[0]);
    close(to_talker[1]);
    assert(0 == unsetenv("RMW_TICKLE_PUBLISH_HUGEPAGE"));
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0); // each case's line survives an assert in the next
    array3m_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    assert(0 == setenv("RMW_TICKLE_LARGE_REGION", "0", 1)); // the context's own buffers, not a shared region's chunks
    run(ON);
    run(OFF);
    printf("test_publish_hugepage: PASS\n");
    return 0;
}
