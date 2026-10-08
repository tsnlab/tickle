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
#include "rmw/time.h"
#include "rmw/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define TOPIC "/loan_bench"
#define NS_PER_S 1000000000ULL
#define US_PER_S 1000000ULL
#define WAIT_TIMEOUT_NS 100000000ULL // a wait that times out, so a silent run still ends
#define MATCH_POLL_NS 1000000L
#define ARRAY_BYTES 1024
#define RELIABLE_DEPTH 1000 // docs/TESTING.md section 5: RELIABLE + KEEP_ALL, the depth perf_test echoes
#define ARGC_WANTED 7       // the program name and the six arguments below
#define ARG_ROLE 1
#define ARG_MODE 2
#define ARG_QOS 3
#define ARG_SECONDS 4
#define ARG_WARM 5
#define ARG_COOL 6

struct array1k {
    uint8_t array[ARRAY_BYTES];
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
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - <time.h> above; see rmw_wait_set.c
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static uint64_t cpu_us(void) {
    struct rusage usage; // NOLINT(misc-include-cleaner) - <sys/resource.h> above
    getrusage(RUSAGE_SELF, &usage);
    return (((uint64_t)usage.ru_utime.tv_sec + (uint64_t)usage.ru_stime.tv_sec) * US_PER_S) +
           (uint64_t)usage.ru_utime.tv_usec + (uint64_t)usage.ru_stime.tv_usec;
}

// The measured window: opens WARM seconds after it starts, closes at SECONDS - COOL; the run ends at SECONDS.
struct window {
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

static void window_tick(struct window* win, uint64_t now, uint64_t count) {
    if (!win->opened && now >= win->open_ns) {
        win->opened = true;
        win->opened_at_ns = now;
        win->count_at_open = count;
        win->cpu_at_open = cpu_us();
    }
    if (win->opened && !win->closed && now >= win->close_ns) {
        win->closed = true;
        win->closed_at_ns = now;
        win->count_at_close = count;
        win->cpu_at_close = cpu_us();
    }
}

static int fail(const char* what) {
    fprintf(stderr, "rmw_loan_bench: %s\n", what);
    printf("RESULT ok=0 why=%s\n", what);
    return 1;
}

// One run's arguments and what it counted.
struct run {
    bool loan;
    double seconds;
    double warm;
    double cool;
    struct window win;
    uint64_t end_ns;
    uint64_t count;
    uint64_t failed;
    uint64_t lost;
    bool can_loan;
};

// (Re)starts the run's window and its end at `start`.
static void run_start(struct run* run, uint64_t start) {
    run->win.open_ns = start + (uint64_t)(run->warm * (double)NS_PER_S);
    run->win.close_ns = start + (uint64_t)((run->seconds - run->cool) * (double)NS_PER_S);
    run->end_ns = start + (uint64_t)(run->seconds * (double)NS_PER_S);
}

static const char* run_publisher(rmw_node_t* node, const rmw_qos_profile_t* qos, struct run* run) {
    rmw_publisher_options_t pub_options = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, &handle, TOPIC, qos, &pub_options);
    if (NULL == pub) {
        return "create_publisher";
    }
    run->can_loan = pub->can_loan_messages;
    if (run->loan && !run->can_loan) {
        return "loan_mode_without_loans";
    }
    size_t matched = 0;
    while (0 == matched && now_ns() < run->end_ns) {
        (void)rmw_publisher_count_matched_subscriptions(pub, &matched);
        struct timespec pause = {0, MATCH_POLL_NS};
        nanosleep(&pause, NULL);
    }
    // The window starts once matched, so a slow discovery does not eat into it.
    run_start(run, now_ns());
    struct array1k own;
    memset(&own, 0, sizeof(own));
    uint64_t id = 0;
    for (uint64_t now = now_ns(); now < run->end_ns; now = now_ns()) {
        window_tick(&run->win, now, run->count);
        id++;
        rmw_ret_t ret = RMW_RET_ERROR;
        if (run->loan) {
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
        run->count += RMW_RET_OK == ret ? 1U : 0U;
        run->failed += RMW_RET_OK == ret ? 0U : 1U;
    }
    window_tick(&run->win, run->end_ns, run->count);
    (void)rmw_destroy_publisher(node, pub);
    return NULL;
}

// Takes one message, by loan or into `own`; true if there was one, its id in *got_id. Both modes read the same fields.
static bool take_one(rmw_subscription_t* sub, bool loan, struct array1k* own, uint64_t* got_id) {
    bool taken = false;
    if (loan) {
        void* message = NULL;
        (void)rmw_take_loaned_message(sub, &message, &taken, NULL);
        if (taken) {
            *got_id = ((const struct array1k*)message)->id;
            own->time = ((const struct array1k*)message)->time;
            (void)rmw_return_loaned_message_from_subscription(sub, message);
        }
    } else {
        (void)rmw_take(sub, own, &taken, NULL);
        *got_id = own->id;
    }
    return taken;
}

static const char* run_subscriber(rmw_context_t* context, rmw_node_t* node, const rmw_qos_profile_t* qos,
                                  struct run* run) {
    rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &handle, TOPIC, qos, &sub_options);
    if (NULL == sub) {
        return "create_subscription";
    }
    run->can_loan = sub->can_loan_messages;
    if (run->loan && !run->can_loan) {
        return "loan_mode_without_loans";
    }
    rmw_wait_set_t* wait_set = rmw_create_wait_set(context, 1);
    void* sub_handles[1];
    struct array1k own;
    uint64_t last_id = 0;
    bool first = true;
    // The subscriber's window starts at its first sample, so the publisher's discovery does not count against it.
    for (uint64_t now = now_ns(); now < run->end_ns; now = now_ns()) {
        if (first && run->count > 0) {
            first = false;
            run_start(run, now);
        }
        window_tick(&run->win, now, run->count);
        sub_handles[0] = sub->data;
        rmw_subscriptions_t subscriptions = {.subscriber_count = 1, .subscribers = sub_handles};
        rmw_time_t timeout = {.sec = 0, .nsec = WAIT_TIMEOUT_NS};
        if (RMW_RET_OK != rmw_wait(&subscriptions, NULL, NULL, NULL, NULL, wait_set, &timeout)) {
            continue; // a timeout: the loop decides whether the run is over
        }
        uint64_t got_id = 0;
        while (take_one(sub, run->loan, &own, &got_id)) {
            run->lost += (last_id != 0 && got_id > last_id + 1) ? got_id - last_id - 1 : 0;
            last_id = got_id;
            run->count++;
        }
    }
    window_tick(&run->win, run->end_ns, run->count);
    (void)rmw_destroy_wait_set(wait_set);
    (void)rmw_destroy_subscription(node, sub);
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != ARGC_WANTED) {
        fprintf(stderr, "usage: rmw_loan_bench pub|sub copy|loan reliable|best_effort SECONDS WARM COOL\n");
        return 2;
    }
    bool publisher = 0 == strcmp(argv[ARG_ROLE], "pub");
    bool reliable = 0 == strcmp(argv[ARG_QOS], "reliable");
    struct run run = {.loan = 0 == strcmp(argv[ARG_MODE], "loan"),
                      .seconds = atof(argv[ARG_SECONDS]),
                      .warm = atof(argv[ARG_WARM]),
                      .cool = atof(argv[ARG_COOL])};
    run_start(&run, now_ns());
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
    qos.depth = reliable ? RELIABLE_DEPTH : 1;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;

    const char* why = publisher ? run_publisher(node, &qos, &run) : run_subscriber(&context, node, &qos, &run);
    if (NULL != why) {
        return fail(why);
    }
    (void)rmw_destroy_node(node);
    (void)rmw_shutdown(&context);
    (void)rmw_context_fini(&context);

    const struct window* win = &run.win;
    if (!win->opened || !win->closed) {
        return fail("window_never_closed");
    }
    uint64_t messages = win->count_at_close - win->count_at_open;
    double window_s = (double)(win->closed_at_ns - win->opened_at_ns) / (double)NS_PER_S;
    uint64_t cpu = win->cpu_at_close - win->cpu_at_open;
    printf("RESULT ok=1 role=%s mode=%s qos=%s can_loan=%d messages=%llu window_s=%.3f rate=%.0f cpu_us=%llu "
           "cpu_us_per_msg=%.3f failed=%llu lost=%llu\n",
           argv[ARG_ROLE], argv[ARG_MODE], argv[ARG_QOS], run.can_loan ? 1 : 0, (unsigned long long)messages, window_s,
           window_s > 0 ? (double)messages / window_s : 0.0, (unsigned long long)cpu,
           messages > 0 ? (double)cpu / (double)messages : 0.0, (unsigned long long)run.failed,
           (unsigned long long)run.lost);
    return 0;
}
