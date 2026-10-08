/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_loan_bench - rmw-level same-host throughput of an Array1k-shaped type with and without loaned messages
// (rmw_tickle docs/RMW.md, "Loaned messages"). Driven by rmw_loan_bench.sh, which says how to read it.
//
//   rmw_loan_bench pub|sub copy|loan reliable|best_effort SECONDS WARM COOL
//
// pub: publishes as fast as it can for SECONDS once matched - copy: fills the id and time of one message of its own
//      and rmw_publish()es it; loan: rmw_borrow_loaned_message(), fills the id and time, rmw_publish_loaned_message().
// sub: rmw_wait() on the subscription, then takes everything there - copy: rmw_take() into a message of its own;
//      loan: rmw_take_loaned_message(), reads the id and time where they lie, returns it. Both read the same two
//      fields, as perf_test's subscriber does.
// Each prints one RESULT line: messages and CPU (getrusage, every thread of the process) over the window that skips
// the first WARM and the last COOL seconds of its own run. The rmw API only, nothing of rmw_tickle's internals: the
// type is hand-written here with the callbacks a generated Array1k has (inplace_bytes 1040, one memcpy codec).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/resource.h>
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
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define TOPIC "/loan_bench"
#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000ULL
#define WAIT_TIMEOUT_NS 100000000ULL // a wait that times out, so a silent run still ends
#define MATCH_POLL_NS 1000000L

struct array1k {
    uint8_t array[1024];
    int64_t time;
    uint64_t id;
};

static int32_t encode_size(const void* ros_message) {
    (void)ros_message;
    return (int32_t)sizeof(struct array1k);
}
static int32_t encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    if (len < sizeof(struct array1k)) {
        return -1;
    }
    memcpy(payload, ros_message, sizeof(struct array1k));
    return (int32_t)sizeof(struct array1k);
}
static int32_t decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct array1k)) {
        return -1;
    }
    memcpy(ros_message, payload, sizeof(struct array1k));
    return (int32_t)sizeof(struct array1k);
}
static int32_t refuse_encode_size(struct tt_Data* data) {
    (void)data;
    return -1;
}

static rosidl_typesupport_tickle_c_message_callbacks_t callbacks = {
    .ros_type_name = "rmw_loan_bench/msg/Array1k",
    .tickle_struct_size = sizeof(struct array1k),
    .ros_struct_size = sizeof(struct array1k),
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&refuse_encode_size,
    .tickle_max_encoded_size = sizeof(struct array1k),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
    .direct_encode_size = &encode_size,
    .direct_encode = &encode,
    .direct_decode = &decode,
    .inplace_bytes = sizeof(struct array1k),
    .ros_struct_align = _Alignof(struct array1k),
};
static rosidl_message_type_support_t handle = {.data = &callbacks, .func = get_message_typesupport_handle_function};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static uint64_t cpu_us(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return ((uint64_t)usage.ru_utime.tv_sec + (uint64_t)usage.ru_stime.tv_sec) * 1000000ULL +
           (uint64_t)usage.ru_utime.tv_usec + (uint64_t)usage.ru_stime.tv_usec;
}

// The measured window: entered once `elapsed` passes WARM, left at SECONDS - COOL.
struct window {
    uint64_t start_ns;
    uint64_t open_ns;
    uint64_t close_ns;
    uint64_t opened_at_ns;
    uint64_t closed_at_ns;
    uint64_t count_at_open;
    uint64_t count_at_close;
    uint64_t cpu_at_open;
    uint64_t cpu_at_close;
    bool opened;
    bool closed;
};

static void window_tick(struct window* w, uint64_t now, uint64_t count) {
    if (!w->opened && now >= w->open_ns) {
        w->opened = true;
        w->opened_at_ns = now;
        w->count_at_open = count;
        w->cpu_at_open = cpu_us();
    }
    if (w->opened && !w->closed && now >= w->close_ns) {
        w->closed = true;
        w->closed_at_ns = now;
        w->count_at_close = count;
        w->cpu_at_close = cpu_us();
    }
}

static int fail(const char* what) {
    fprintf(stderr, "rmw_loan_bench: %s\n", what);
    printf("RESULT ok=0 why=%s\n", what);
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: rmw_loan_bench pub|sub copy|loan reliable|best_effort SECONDS WARM COOL\n");
        return 2;
    }
    bool publisher = 0 == strcmp(argv[1], "pub");
    bool loan = 0 == strcmp(argv[2], "loan");
    bool reliable = 0 == strcmp(argv[3], "reliable");
    double seconds = atof(argv[4]);
    double warm = atof(argv[5]);
    double cool = atof(argv[6]);
    handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;

    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    (void)rmw_init_options_init(&options, allocator);
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    if (RMW_RET_OK != rmw_init(&options, &context)) {
        return fail("rmw_init");
    }
    rmw_node_t* node = rmw_create_node(&context, publisher ? "loan_bench_pub" : "loan_bench_sub", "/");
    // The QoS docs/TESTING.md section 5 sets for throughput cells: RELIABLE + KEEP_ALL, BEST_EFFORT + KEEP_LAST 1.
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.reliability = reliable ? RMW_QOS_POLICY_RELIABILITY_RELIABLE : RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    qos.history = reliable ? RMW_QOS_POLICY_HISTORY_KEEP_ALL : RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    qos.depth = reliable ? 1000 : 1;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;

    uint64_t start = now_ns();
    struct window w = {.start_ns = start,
                       .open_ns = start + (uint64_t)(warm * (double)NS_PER_S),
                       .close_ns = start + (uint64_t)((seconds - cool) * (double)NS_PER_S)};
    uint64_t end_ns = start + (uint64_t)(seconds * (double)NS_PER_S);
    uint64_t count = 0;
    uint64_t failed = 0;
    uint64_t lost = 0;
    bool can_loan = false;

    if (publisher) {
        rmw_publisher_options_t pub_options = rmw_get_default_publisher_options();
        rmw_publisher_t* pub = rmw_create_publisher(node, &handle, TOPIC, &qos, &pub_options);
        if (NULL == pub) {
            return fail("create_publisher");
        }
        can_loan = pub->can_loan_messages;
        if (loan && !can_loan) {
            return fail("loan_mode_without_loans");
        }
        size_t matched = 0;
        while (0 == matched && now_ns() < end_ns) {
            (void)rmw_publisher_count_matched_subscriptions(pub, &matched);
            struct timespec pause = {0, MATCH_POLL_NS};
            nanosleep(&pause, NULL);
        }
        // The window starts once matched, so a slow discovery does not eat into it.
        start = now_ns();
        w.open_ns = start + (uint64_t)(warm * (double)NS_PER_S);
        w.close_ns = start + (uint64_t)((seconds - cool) * (double)NS_PER_S);
        end_ns = start + (uint64_t)(seconds * (double)NS_PER_S);
        struct array1k own;
        memset(&own, 0, sizeof(own));
        uint64_t id = 0;
        for (uint64_t now = now_ns(); now < end_ns; now = now_ns()) {
            window_tick(&w, now, count);
            id++;
            rmw_ret_t ret = RMW_RET_ERROR;
            if (loan) {
                void* message = NULL;
                ret = rmw_borrow_loaned_message(pub, &handle, &message);
                if (RMW_RET_OK == ret) {
                    ((struct array1k*)message)->id = id;
                    ((struct array1k*)message)->time = (int64_t)now;
                    ret = rmw_publish_loaned_message(pub, message, NULL);
                }
            } else {
                own.id = id;
                own.time = (int64_t)now;
                ret = rmw_publish(pub, &own, NULL);
            }
            count += RMW_RET_OK == ret ? 1U : 0U;
            failed += RMW_RET_OK == ret ? 0U : 1U;
        }
        window_tick(&w, end_ns, count);
        (void)rmw_destroy_publisher(node, pub);
    } else {
        rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
        rmw_subscription_t* sub = rmw_create_subscription(node, &handle, TOPIC, &qos, &sub_options);
        if (NULL == sub) {
            return fail("create_subscription");
        }
        can_loan = sub->can_loan_messages;
        if (loan && !can_loan) {
            return fail("loan_mode_without_loans");
        }
        rmw_wait_set_t* wait_set = rmw_create_wait_set(&context, 1);
        void* sub_handles[1];
        struct array1k own;
        uint64_t last_id = 0;
        uint64_t first_ns = 0;
        // The subscriber's window starts at its first sample, so the publisher's discovery does not count against it.
        for (uint64_t now = now_ns(); now < end_ns; now = now_ns()) {
            if (0 == first_ns && count > 0) {
                first_ns = now;
                w.open_ns = now + (uint64_t)(warm * (double)NS_PER_S);
                w.close_ns = now + (uint64_t)((seconds - cool) * (double)NS_PER_S);
                end_ns = now + (uint64_t)(seconds * (double)NS_PER_S);
            }
            window_tick(&w, now, count);
            sub_handles[0] = sub->data;
            rmw_subscriptions_t subscriptions = {.subscriber_count = 1, .subscribers = sub_handles};
            rmw_time_t timeout = {.sec = 0, .nsec = WAIT_TIMEOUT_NS};
            rmw_ret_t wait_ret = rmw_wait(&subscriptions, NULL, NULL, NULL, NULL, wait_set, &timeout);
            if (RMW_RET_OK != wait_ret) {
                continue; // a timeout: the loop decides whether the run is over
            }
            bool taken = true;
            while (taken) {
                uint64_t got_id = 0;
                if (loan) {
                    void* message = NULL;
                    taken = false;
                    (void)rmw_take_loaned_message(sub, &message, &taken, NULL);
                    if (taken) {
                        got_id = ((const struct array1k*)message)->id;
                        own.time = ((const struct array1k*)message)->time;
                        (void)rmw_return_loaned_message_from_subscription(sub, message);
                    }
                } else {
                    taken = false;
                    (void)rmw_take(sub, &own, &taken, NULL);
                    got_id = own.id;
                }
                if (taken) {
                    lost += (last_id != 0 && got_id > last_id + 1) ? got_id - last_id - 1 : 0;
                    last_id = got_id;
                    count++;
                }
            }
        }
        window_tick(&w, end_ns, count);
        (void)rmw_destroy_wait_set(wait_set);
        (void)rmw_destroy_subscription(node, sub);
    }
    (void)rmw_destroy_node(node);
    (void)rmw_shutdown(&context);
    (void)rmw_context_fini(&context);

    if (!w.opened || !w.closed) {
        return fail("window_never_closed");
    }
    uint64_t messages = w.count_at_close - w.count_at_open;
    double window_s = (double)(w.closed_at_ns - w.opened_at_ns) / (double)NS_PER_S;
    uint64_t cpu = w.cpu_at_close - w.cpu_at_open;
    printf("RESULT ok=1 role=%s mode=%s qos=%s can_loan=%d messages=%llu window_s=%.3f rate=%.0f cpu_us=%llu "
           "cpu_us_per_msg=%.3f failed=%llu lost=%llu\n",
           argv[1], argv[2], argv[3], can_loan ? 1 : 0, (unsigned long long)messages, window_s,
           window_s > 0 ? (double)messages / window_s : 0.0, (unsigned long long)cpu,
           messages > 0 ? (double)cpu / (double)messages : 0.0, (unsigned long long)failed, (unsigned long long)lost);
    return 0;
}
