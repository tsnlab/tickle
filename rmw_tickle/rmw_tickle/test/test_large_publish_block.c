/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS (docs/RMW.md, "Large samples: blocking publish").
//
// Core's publish of a large sample never blocks: the send goes on from the poll, and a sample published meanwhile
// waits behind it and is replaced by a newer one (KEEP_LAST at the sender, core's large.tail_abandoned). A DDS publish
// blocks in the send instead. The option makes rmw_publish() wait, before core sees the sample, while the same
// publisher's previous large sample is still going. Two processes, 4 MB samples (perf_test's Array4m) published back
// to back, so the link is saturated; BEST_EFFORT KEEP_LAST depth 5, as the rig's I4s cell:
//
// off: the option unset, the control. No publish waits, and core replaces samples at the sender (abandoned >= 1).
//
// on: RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS=5000. Publishes wait (waits >= 1, timeouts 0) and nothing is abandoned at the
// sender. A small-type publisher on the same context, publishing while the last large sample is still going, never
// waits: its large_block_ns is 0, and the wait count does not move across its publish.
//
// timeout: =1. The waits run out (timeouts >= 1), every publish still returns OK, and core applies KEEP_LAST
// (abandoned >= 1): the option bounds the wait and never fails a KEEP_LAST write.
//
// executor: =5000 with executor-driven receive in play. The publishing thread calls rmw_wait() between publishes
// (taking the poll role and leaving its lease, as a single-threaded executor does), and a second thread spins
// rmw_wait() throughout. No deadlock, no timeout, nothing abandoned.
//
// What arrives is printed, not judged: best effort on a shared PC loses what it loses at the reader.
//
// The link: a full send buffer is what makes a large send wait (tickle.c large_wait_for_room()). Loopback never fills
// one, and two processes on one address are same-host peers, whose large fragments go another way. So the talker runs
// in a network namespace of its own, joined to the listener's by a veth pair whose talker end tbf shapes to the rig's
// 1 Gbit/s - the PC experiments' set-up (experiments/large_saturation_pc.sh) - and the two find each other by broadcast
// on 10.77.0.0/24. That needs `sudo -n ip` and a named private namespace to start in (run_rmw_suite.sh's): in the
// default one it would add an interface to the machine. Anywhere else the test says why and exits 77, which CMake
// reports as skipped, not passed.

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
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

#define ARRAY4M_BYTES ((size_t)4 * 1024 * 1024)
#define SAMPLES 12U
#define DEPTH 5U
#define POLL_US 200L
#define WAIT_POLLS 50000 // 10 s at POLL_US
#define NS_PER_US 1000L
#define NS_PER_MS 1000000ULL
#define SKIP_CODE 77
#define SMALL_BYTES 64U
#define SETTLE_POLLS 15000 // 3 s at POLL_US: the last samples after the talker said it was done
// The shaped link, on the talker's veth end: the rig's 1 Gbit/s, a 4 MB sample ~34 ms. Its queue is shorter than the
// send buffer (rmw asks for 4 MiB, 8 MiB once the kernel doubles it), so the sender's buffer fills within a sample or
// two; twelve keep it full.
#define TBF_ARGS "rate 1gbit burst 64kb latency 100ms"
#define LINK_PREFIX "10.77.0."
#define LINK_BROADCAST LINK_PREFIX "255"

struct array4m {
    uint8_t array[ARRAY4M_BYTES];
    int64_t time;
    uint64_t id;
};

struct small {
    uint8_t bytes[SMALL_BYTES - sizeof(uint64_t)];
    uint64_t id;
};

static int32_t array4m_encode_size(const void* ros_message) {
    (void)ros_message;
    return (int32_t)sizeof(struct array4m);
}

static int32_t array4m_encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    if (len < sizeof(struct array4m)) {
        return -1;
    }
    memcpy(payload, ros_message, sizeof(struct array4m));
    return (int32_t)sizeof(struct array4m);
}

static int32_t array4m_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct array4m)) {
        return -1;
    }
    memcpy(ros_message, payload, sizeof(struct array4m));
    return (int32_t)sizeof(struct array4m);
}

static int32_t small_encode_size(const void* ros_message) {
    (void)ros_message;
    return (int32_t)sizeof(struct small);
}

static int32_t small_encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    if (len < sizeof(struct small)) {
        return -1;
    }
    memcpy(payload, ros_message, sizeof(struct small));
    return (int32_t)sizeof(struct small);
}

static int32_t small_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct small)) {
        return -1;
    }
    memcpy(ros_message, payload, sizeof(struct small));
    return (int32_t)sizeof(struct small);
}

static int32_t refuse_encode_size(struct tt_Data* data) {
    (void)data;
    return -1;
}

static rosidl_typesupport_tickle_c_message_callbacks_t array4m_callbacks = {
    .ros_type_name = "test_large_publish_block/msg/Array4m",
    .tickle_struct_size = sizeof(struct array4m),
    .ros_struct_size = sizeof(struct array4m),
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&refuse_encode_size,
    .tickle_max_encoded_size = sizeof(struct array4m),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
    .direct_encode_size = &array4m_encode_size,
    .direct_encode = &array4m_encode,
    .direct_decode = &array4m_decode,
    .inplace_bytes = sizeof(struct array4m),
    .ros_struct_align = _Alignof(struct array4m),
};

static rosidl_typesupport_tickle_c_message_callbacks_t small_callbacks = {
    .ros_type_name = "test_large_publish_block/msg/Small",
    .tickle_struct_size = sizeof(struct small),
    .ros_struct_size = sizeof(struct small),
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&refuse_encode_size,
    .tickle_max_encoded_size = sizeof(struct small),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
    .direct_encode_size = &small_encode_size,
    .direct_encode = &small_encode,
    .direct_decode = &small_decode,
    .inplace_bytes = sizeof(struct small),
    .ros_struct_align = _Alignof(struct small),
};

static rosidl_message_type_support_t array4m_handle = {.data = &array4m_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t small_handle = {.data = &small_callbacks,
                                                     .func = get_message_typesupport_handle_function};

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * 1000000000ULL) + (uint64_t)now.tv_nsec;
}

static void sleep_us(long micros) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = micros * NS_PER_US};
    nanosleep(&interval, NULL);
}

static rmw_qos_profile_t qos(void) {
    rmw_qos_profile_t q = rmw_qos_profile_default;
    q.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
    q.depth = DEPTH;
    q.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    q.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    return q;
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
    // Across the veth: rmw_init_options_init() leaves the range LOCALHOST, which would keep both on lo.
    set->options.discovery_options.automatic_discovery_range = RMW_AUTOMATIC_DISCOVERY_RANGE_SUBNET;
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

enum scenario { OFF, ON, TIMEOUT, EXECUTOR, SCENARIOS };
static const char* const scenario_names[] = {"off", "on", "timeout", "executor"};
static const char* const scenario_env[] = {"", "5000", "1", "5000"};
#define TOPIC "/large_publish_block"
#define SMALL_TOPIC "/large_publish_block_small"

// What the talker reports to the listener when it has published everything.
struct report {
    uint64_t abandoned;      // core: large samples replaced or cut short at the sender (large.tail_abandoned)
    uint64_t waits;          // rmw: publishes that waited for the previous large sample
    uint64_t timeouts;       // rmw: waits that ran out
    uint64_t small_waits;    // waits counted across the small publisher's publish
    uint64_t published_ok;   // large publishes that returned RMW_RET_OK
    uint64_t longest_ns;     // the longest large publish
    uint64_t total_ns;       // first publish to last
    uint64_t small_block_ns; // the small publisher's large_block_ns: 0, or it could wait
    uint64_t large_block_ns; // the large publisher's
    uint64_t send_waits;     // core: times a large send found the socket's buffer full (large.send_waits)
    bool small_in_flight;    // a large sample was still going when the small one was published
};

// EXECUTOR: a second thread spinning rmw_wait() on its own guard condition, as another executor would.
struct spinner {
    rmw_context_t* context;
    atomic_bool stop;
    uint64_t waits;
};

static void* spin(void* arg) {
    struct spinner* spinner = arg;
    rmw_guard_condition_t* guard = rmw_create_guard_condition(spinner->context);
    rmw_wait_set_t* wait_set = rmw_create_wait_set(spinner->context, 0);
    assert(NULL != guard && NULL != wait_set);
    while (!atomic_load(&spinner->stop)) {
        void* storage[1] = {guard->data};
        rmw_guard_conditions_t guards = {.guard_condition_count = 1, .guard_conditions = storage};
        rmw_time_t timeout = {.sec = 0, .nsec = 5 * NS_PER_MS};
        rmw_ret_t ret = rmw_wait(NULL, &guards, NULL, NULL, NULL, wait_set, &timeout);
        assert(RMW_RET_OK == ret || RMW_RET_TIMEOUT == ret);
        spinner->waits++;
    }
    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_guard_condition(guard));
    return NULL;
}

// The publishing thread's own executor turn: an rmw_wait() of 1 ms, which polls the node and leaves its lease.
static void executor_turn(rmw_wait_set_t* wait_set, rmw_guard_condition_t* guard) {
    void* storage[1] = {guard->data};
    rmw_guard_conditions_t guards = {.guard_condition_count = 1, .guard_conditions = storage};
    rmw_time_t timeout = {.sec = 0, .nsec = NS_PER_MS};
    rmw_ret_t ret = rmw_wait(NULL, &guards, NULL, NULL, NULL, wait_set, &timeout);
    assert(RMW_RET_OK == ret || RMW_RET_TIMEOUT == ret);
}

static bool wait_matched(rmw_publisher_t* pub) {
    for (int i = 0; i < WAIT_POLLS; i++) {
        size_t count = 0;
        assert(RMW_RET_OK == rmw_publisher_count_matched_subscriptions(pub, &count));
        if (count >= 1) {
            return true;
        }
        sleep_us(POLL_US);
    }
    return false;
}

static bool file_exists(const char* path) {
    return 0 == access(path, F_OK);
}

// Publishes SAMPLES large samples back to back (an executor turn before each in EXECUTOR), then one small one.
static void publish_all(enum scenario which, rmw_context_t* context, rmw_tickle_context_impl_t* context_impl,
                        rmw_publisher_t* pub, rmw_publisher_t* small_pub, struct report* report) {
    struct spinner spinner = {.context = context, .stop = false, .waits = 0};
    pthread_t spinner_thread;
    rmw_guard_condition_t* guard = NULL;
    rmw_wait_set_t* wait_set = NULL;
    if (EXECUTOR == which) {
        assert(0 == pthread_create(&spinner_thread, NULL, spin, &spinner));
        guard = rmw_create_guard_condition(context);
        wait_set = rmw_create_wait_set(context, 0);
        assert(NULL != guard && NULL != wait_set);
    }
    struct array4m* message = calloc(1, sizeof(*message));
    assert(NULL != message);
    uint64_t start = now_ns();
    for (uint64_t id = 1; id <= SAMPLES; id++) {
        if (EXECUTOR == which) {
            executor_turn(wait_set, guard);
        }
        message->id = id;
        message->time = (int64_t)id;
        uint64_t before = now_ns();
        rmw_ret_t ret = rmw_publish(pub, message, NULL);
        uint64_t took = now_ns() - before;
        report->longest_ns = took > report->longest_ns ? took : report->longest_ns;
        report->published_ok += RMW_RET_OK == ret ? 1U : 0U;
    }
    report->total_ns = now_ns() - start;
    free(message);
    // The small publisher, while the last large sample is still going: it must not wait.
    report->small_in_flight = tt_Publisher_large_in_flight(&((rmw_tickle_publisher_t*)pub->data)->tickle_publisher);
    uint64_t waits_before = atomic_load(&context_impl->large_block_waits);
    struct small small_message = {.bytes = {0}, .id = 1};
    assert(RMW_RET_OK == rmw_publish(small_pub, &small_message, NULL));
    report->small_waits = atomic_load(&context_impl->large_block_waits) - waits_before;
    if (EXECUTOR == which) {
        atomic_store(&spinner.stop, true);
        assert(0 == pthread_join(spinner_thread, NULL));
        assert(spinner.waits > 0);
        assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
        assert(RMW_RET_OK == rmw_destroy_guard_condition(guard));
    }
}

// The talker, in its own namespace: SAMPLES large samples once the listener has matched it (go_path appears), one
// report line on stdout, then it stays until the listener is done (done_path), so the last sample is not cut short.
static int talker(enum scenario which, const char* go_path, const char* done_path) {
    struct endpoint_set set;
    open_set(&set, "block_talker");
    rmw_tickle_context_impl_t* context_impl = ((rmw_tickle_node_t*)set.node->data)->context_impl;
    rmw_publisher_options_t options = rmw_get_default_publisher_options();
    rmw_qos_profile_t q = qos();
    rmw_publisher_t* pub = rmw_create_publisher(set.node, &array4m_handle, TOPIC, &q, &options);
    rmw_publisher_t* small_pub = rmw_create_publisher(set.node, &small_handle, SMALL_TOPIC, &q, &options);
    assert(NULL != pub && NULL != small_pub);
    bool go = wait_matched(pub);
    for (int i = 0; i < WAIT_POLLS && go && !file_exists(go_path); i++) {
        sleep_us(POLL_US); // the listener has matched it too: a best-effort reader drops what comes before
    }
    if (!go || !file_exists(go_path)) {
        return 2;
    }
    struct report report = {0};
    report.large_block_ns = ((rmw_tickle_publisher_t*)pub->data)->large_block_ns;
    report.small_block_ns = ((rmw_tickle_publisher_t*)small_pub->data)->large_block_ns;
    publish_all(which, &set.context, context_impl, pub, small_pub, &report);
    tt_Context_lock(&context_impl->tickle_context);
    report.abandoned = context_impl->tickle_context.large.tail_abandoned;
    report.send_waits = context_impl->tickle_context.large.send_waits;
    tt_Context_unlock(&context_impl->tickle_context);
    report.waits = atomic_load(&context_impl->large_block_waits);
    report.timeouts = atomic_load(&context_impl->large_block_timeouts);
    printf("REPORT %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %d\n", (unsigned long long)report.abandoned,
           (unsigned long long)report.waits, (unsigned long long)report.timeouts,
           (unsigned long long)report.small_waits, (unsigned long long)report.published_ok,
           (unsigned long long)report.longest_ns, (unsigned long long)report.total_ns,
           (unsigned long long)report.small_block_ns, (unsigned long long)report.large_block_ns,
           (unsigned long long)report.send_waits, report.small_in_flight ? 1 : 0);
    (void)fflush(stdout);
    for (int i = 0; i < WAIT_POLLS && !file_exists(done_path); i++) {
        sleep_us(POLL_US);
    }
    assert(RMW_RET_OK == rmw_destroy_publisher(set.node, small_pub));
    assert(RMW_RET_OK == rmw_destroy_publisher(set.node, pub));
    close_set(&set);
    return 0;
}

static bool parse_report(const char* line, struct report* report) {
    unsigned long long v[10] = {0};
    int in_flight = 0;
    if (11 != sscanf(line, "REPORT %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %d", &v[0], &v[1], &v[2], &v[3],
                     &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &in_flight)) {
        return false;
    }
    *report = (struct report) {.abandoned = v[0],
                               .waits = v[1],
                               .timeouts = v[2],
                               .small_waits = v[3],
                               .published_ok = v[4],
                               .longest_ns = v[5],
                               .total_ns = v[6],
                               .small_block_ns = v[7],
                               .large_block_ns = v[8],
                               .send_waits = v[9],
                               .small_in_flight = 0 != in_flight};
    return true;
}

// Takes what has arrived; counts distinct ids, in order. False when there was nothing.
static bool take_one(rmw_subscription_t* sub, struct array4m* into, uint64_t* received, uint64_t* last_id) {
    bool taken = false;
    assert(RMW_RET_OK == rmw_take(sub, into, &taken, NULL));
    if (!taken) {
        return false;
    }
    assert(into->id > *last_id && into->id <= SAMPLES); // in order, never twice
    *last_id = into->id;
    (*received)++;
    return true;
}

// A value put on the talker's command line: refused unless plain, so nothing in it can quote its way out.
static bool plain_value(const char* value) {
    for (const char* c = value; *c != '\0'; c++) {
        if (*c == '\'' || *c == '\\' || *c == '\n') {
            return false;
        }
    }
    return true;
}

static char talker_ns[32];
static char exe_path[512];

// The talker: this binary, as the invoking user, in the talker's namespace, with the library paths it needs (sudo
// resets the environment) and the scenario's setting. Its stdout is the returned stream.
static FILE* start_talker(enum scenario which, const char* go_path, const char* done_path) {
    static const char* const kept[] = {"LD_LIBRARY_PATH", "AMENT_PREFIX_PATH", "PATH", "TICKLE_ID_REGISTRY",
                                       "ROS_DOMAIN_ID"};
    char command[32768];
    const char* user = getenv("USER");
    if (NULL == user || !plain_value(user)) {
        return NULL;
    }
    size_t used = 0;
    int wrote = snprintf(command, sizeof(command), "sudo -n ip netns exec %s sudo -n -u '%s' env", talker_ns, user);
    used = wrote > 0 ? (size_t)wrote : sizeof(command);
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]) && used < sizeof(command); i++) {
        const char* value = getenv(kept[i]);
        if (NULL != value && plain_value(value)) {
            wrote = snprintf(command + used, sizeof(command) - used, " %s='%s'", kept[i], value);
            used += wrote > 0 ? (size_t)wrote : sizeof(command);
        }
    }
    if (used < sizeof(command)) {
        wrote = snprintf(command + used, sizeof(command) - used,
                         " TICKLE_BROADCAST_ADDR=" LINK_BROADCAST
                         " RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS='%s' '%s' --talker %d '%s' '%s'",
                         scenario_env[which], exe_path, (int)which, go_path, done_path);
        used += wrote > 0 ? (size_t)wrote : sizeof(command);
    }
    if (used >= sizeof(command)) {
        return NULL;
    }
    return popen(command, "r"); // NOLINT(cert-env33-c) - every value checked by plain_value()
}

static bool read_report(FILE* from_talker, struct report* report) {
    char line[512];
    while (NULL != fgets(line, (int)sizeof(line), from_talker)) {
        if (parse_report(line, report)) {
            return true;
        }
    }
    clearerr(from_talker); // non-blocking: nothing more yet
    return false;
}

static void check(enum scenario which, const struct report* report, uint64_t received) {
    printf("%s: published %llu of %u OK in %.1f ms (longest %.1f ms); waits %llu, timeouts %llu, abandoned at the "
           "sender %llu, send_waits %llu; received %llu; small publisher: block_ns %llu, waits %llu, large in "
           "flight %d\n",
           scenario_names[which], (unsigned long long)report->published_ok, SAMPLES, (double)report->total_ns / 1e6,
           (double)report->longest_ns / 1e6, (unsigned long long)report->waits, (unsigned long long)report->timeouts,
           (unsigned long long)report->abandoned, (unsigned long long)report->send_waits, (unsigned long long)received,
           (unsigned long long)report->small_block_ns, (unsigned long long)report->small_waits,
           report->small_in_flight ? 1 : 0);
    assert(SAMPLES == report->published_ok); // KEEP_LAST never fails a publish, on, off or timed out
    assert(report->send_waits >= 1);         // the link was saturated, or this tested nothing
    assert(0 == report->small_block_ns && 0 == report->small_waits);
    switch (which) {
    case OFF:
        assert(0 == report->large_block_ns);
        assert(0 == report->waits && 0 == report->timeouts);
        assert(report->abandoned >= 1); // what the option exists to remove, or the control controls nothing
        break;
    case ON:
    case EXECUTOR:
        assert(5000ULL * NS_PER_MS == report->large_block_ns);
        assert(report->waits >= 1);
        assert(0 == report->timeouts);
        assert(0 == report->abandoned);
        assert(received >= 1);
        assert(report->small_in_flight); // the small publish met a large send in progress, or it tested nothing
        break;
    case TIMEOUT:
        assert(NS_PER_MS == report->large_block_ns);
        assert(report->timeouts >= 1);
        assert(report->abandoned >= 1);
        break;
    case SCENARIOS:
        assert(false);
    }
    printf("%s: ok\n", scenario_names[which]);
}

static void run(enum scenario which) {
    char go_path[256];
    char done_path[256];
    const char* tmp = NULL != getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    (void)snprintf(go_path, sizeof(go_path), "%s/lpb_go_%d_%d", tmp, (int)getpid(), (int)which);
    (void)snprintf(done_path, sizeof(done_path), "%s/lpb_done_%d_%d", tmp, (int)getpid(), (int)which);
    assert(plain_value(go_path) && plain_value(done_path));
    (void)unlink(go_path);
    (void)unlink(done_path);
    FILE* from_talker = start_talker(which, go_path, done_path);
    assert(NULL != from_talker);
    assert(0 == fcntl(fileno(from_talker), F_SETFL, O_NONBLOCK));

    struct endpoint_set set;
    open_set(&set, "block_listener");
    rmw_subscription_options_t options = rmw_get_default_subscription_options();
    rmw_qos_profile_t q = qos();
    rmw_subscription_t* sub = rmw_create_subscription(set.node, &array4m_handle, TOPIC, &q, &options);
    rmw_subscription_t* small_sub = rmw_create_subscription(set.node, &small_handle, SMALL_TOPIC, &q, &options);
    assert(NULL != sub && NULL != small_sub);

    // Best effort drops what arrives before the reader knows the writer: the talker starts once it does.
    bool matched = false;
    for (int i = 0; i < WAIT_POLLS && !matched; i++) {
        size_t count = 0;
        assert(RMW_RET_OK == rmw_subscription_count_matched_publishers(sub, &count));
        matched = count >= 1;
        if (!matched) {
            sleep_us(POLL_US);
        }
    }
    assert(matched);
    FILE* go = fopen(go_path, "w");
    assert(NULL != go && 0 == fclose(go));

    struct array4m* into = malloc(sizeof(*into));
    assert(NULL != into);
    uint64_t received = 0;
    uint64_t last_id = 0;
    struct report report;
    bool reported = false;
    int settle = 0;
    for (int i = 0; i < 4 * WAIT_POLLS && received < SAMPLES && settle < SETTLE_POLLS; i++) {
        reported = reported || read_report(from_talker, &report);
        settle += reported ? 1 : 0;
        if (!take_one(sub, into, &received, &last_id)) {
            sleep_us(POLL_US);
        }
    }
    for (int i = 0; i < WAIT_POLLS && !reported; i++) {
        reported = read_report(from_talker, &report);
        sleep_us(POLL_US);
    }
    free(into);
    FILE* done = fopen(done_path, "w");
    assert(NULL != done && 0 == fclose(done));
    assert(0 == fcntl(fileno(from_talker), F_SETFL, 0));
    char line[512];
    while (NULL != fgets(line, (int)sizeof(line), from_talker)) {
    }
    int status = pclose(from_talker);
    (void)unlink(go_path);
    (void)unlink(done_path);
    assert(WIFEXITED(status) && 0 == WEXITSTATUS(status));
    assert(reported);
    check(which, &report, received);

    assert(RMW_RET_OK == rmw_destroy_subscription(set.node, small_sub));
    assert(RMW_RET_OK == rmw_destroy_subscription(set.node, sub));
    close_set(&set);
}

// Whether this process runs in a named network namespace (`ip netns identify`); false in the default one.
static bool in_named_netns(void) {
    FILE* identify = popen("ip netns identify 2>/dev/null", "r");
    if (NULL == identify) {
        return false;
    }
    char name[128];
    bool found = NULL != fgets(name, (int)sizeof(name), identify);
    (void)pclose(identify);
    return found && name[0] != '\n' && name[0] != '\0';
}

static int ip_command(const char* format, const char* first, const char* second) {
    char command[512];
    (void)snprintf(command, sizeof(command), format, first, second);
    return system(command); // NOLINT(cert-env33-c) - fixed commands; the names are this test's own
}

static void delete_talker_ns(void) {
    (void)ip_command("sudo -n ip netns del %s 2>/dev/null%s", talker_ns, ""); // its veth end goes, and the pair
}

// The talker's namespace and the veth pair to it, its end shaped. False when any step is refused.
static bool build_talker_ns(void) {
    char ours[16];
    char theirs[16];
    (void)snprintf(talker_ns, sizeof(talker_ns), "lpbtalk%d", (int)getpid());
    (void)snprintf(ours, sizeof(ours), "lpb%da", (int)(getpid() % 100000));
    (void)snprintf(theirs, sizeof(theirs), "lpb%db", (int)(getpid() % 100000));
    return 0 == ip_command("sudo -n ip netns add %s%s", talker_ns, "") &&
           0 == ip_command("sudo -n ip link add %s type veth peer name %s", ours, theirs) &&
           0 == ip_command("sudo -n ip link set %s netns %s", theirs, talker_ns) &&
           0 == ip_command("sudo -n ip addr add " LINK_PREFIX "1/24 broadcast " LINK_BROADCAST " dev %s%s", ours, "") &&
           0 == ip_command("sudo -n ip link set %s up%s", ours, "") &&
           0 == ip_command("sudo -n ip -n %s addr add " LINK_PREFIX "2/24 broadcast " LINK_BROADCAST " dev %s",
                           talker_ns, theirs) &&
           0 == ip_command("sudo -n ip -n %s link set %s up", talker_ns, theirs) &&
           0 == ip_command("sudo -n ip -n %s link set lo up%s", talker_ns, "") &&
           0 == ip_command("sudo -n ip netns exec %s tc qdisc add dev %s root tbf " TBF_ARGS, talker_ns, theirs);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0); // each case's line survives an assert in the next
    array4m_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    small_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    if (argc == 5 && 0 == strcmp(argv[1], "--talker")) {
        int which = atoi(argv[2]);
        assert(which >= 0 && which < (int)SCENARIOS);
        return talker((enum scenario)which, argv[3], argv[4]);
    }
    if (!in_named_netns()) {
        printf("test_large_publish_block: SKIP - not in a named private network namespace, where it may add a veth "
               "pair (run it through run_rmw_suite.sh)\n");
        return SKIP_CODE;
    }
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    assert(len > 0);
    exe_path[len] = '\0';
    assert(plain_value(exe_path));
    assert(0 == atexit(delete_talker_ns)); // an assert's abort() skips it: the namespace is named for this pid
    if (!build_talker_ns()) {
        printf("test_large_publish_block: SKIP - cannot build the talker's namespace and link (needs sudo -n ip)\n");
        return SKIP_CODE;
    }
    assert(0 == setenv("TICKLE_BROADCAST_ADDR", LINK_BROADCAST, 1)); // the listener's link: the veth, not lo
    for (int which = 0; which < (int)SCENARIOS; which++) {
        run((enum scenario)which);
    }
    printf("test_large_publish_block: PASS\n");
    return 0;
}
