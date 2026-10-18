/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// QoS plan D1 (tt_VERSION 14): a subscription drops a sample when its age reaches the shorter of its writer's lifespan
// and its own. Published through rmw_publish() to a subscription of the same process (local delivery), taken twice per
// arm: at once (it must be there - else "dropped" later could be "never arrived") and LATER_MS after a second publish
// (it must be gone exactly when the shorter lifespan is under LATER_MS):
//
//   arm                    writer   reader   later
//   neither (control)      -        -        taken
//   long both (control)    3 s      3 s      taken
//   writer only            SHORT    -        dropped   <- the reader's lifespan alone (before D1) keeps it
//   reader only            -        SHORT    dropped   <- the writer's alone loses it
//   writer shorter         SHORT    3 s      dropped   <- max() instead of min() keeps it
//   reader shorter         3 s      SHORT    dropped
//
// The clock offset (the user's point: hosts' clocks differ, so a one-way age cannot be trusted). The age is the
// reader's date minus the writer's source timestamp, as in DDS, so across hosts a lifespan is off by the clock offset.
// Shown with the writer's lifespan alone (OFFSET_LIFESPAN_MS), samples handed to the subscription as core delivers
// them, from that writer, stamped as a writer clock at an offset would stamp them, taken OFFSET_LATER_MS later:
//   offset 0 (same host, the control)        -> taken
//   writer clock OFFSET_MS behind the reader  -> dropped: it arrives looking OFFSET_MS old
//   writer clock 1 h ahead                    -> taken: a stamp ahead is age 0, not an unsigned wrap to "expired"
//
// Mutants, each killed here at the line named: lifespan_expiry() using the subscription's own lifespan only ("writer
// only"); core answering the writer's only ("reader only"); max for min ("writer shorter"); the take loop checking the
// subscription's own lifespan_ns instead of the entry's expires_ns ("writer only"); no age on arrival (offset behind).

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <tickle/hal.h>
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
#include "rmw_tickle_c/rmw_tickle.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

struct pair {
    uint32_t first;
    uint32_t second;
};

static bool convert(const void* source, void* dest) {
    *(struct pair*)dest = *(const struct pair*)source;
    return true;
}
static int32_t encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(struct pair);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    (void)len;
    memcpy(payload, data, sizeof(struct pair));
    return (int32_t)sizeof(struct pair);
}
static int32_t decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct pair)) {
        return -1;
    }
    memcpy(data, payload, sizeof(struct pair));
    return (int32_t)sizeof(struct pair);
}
static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t pair_callbacks = {
    .ros_type_name = "lifespan_writer/msg/Pair",
    .tickle_struct_size = sizeof(struct pair),
    .ros_struct_size = sizeof(struct pair),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&convert,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&convert,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&encode,
    .tickle_decode = (tt_DATA_DECODE)&decode,
    .tickle_free = (tt_DATA_FREE)&free_nothing,
    .tickle_max_encoded_size = sizeof(struct pair),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
};
static rosidl_message_type_support_t pair_handle = {.data = &pair_callbacks,
                                                    .func = get_message_typesupport_handle_function};

#define MS_NS 1000000ULL
#define MS_PER_SEC 1000ULL
#define TOPIC_BYTES 64
#define SHORT_MS 150ULL
#define LONG_MS 3000ULL
#define LATER_MS 400L
#define ARRIVAL_WAIT_MS 1000L
#define OFFSET_LIFESPAN_MS 500ULL
#define OFFSET_MS 400ULL
#define OFFSET_LATER_MS 200L
#define AHEAD_MS 3600000ULL

static void sleep_ms(long millis) {
    struct timespec delay = {.tv_sec = millis / (long)MS_PER_SEC, .tv_nsec = (millis % (long)MS_PER_SEC) * (long)MS_NS};
    nanosleep(&delay, NULL);
}

static size_t queued(rmw_subscription_t* sub) {
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    pthread_mutex_lock(&sub_impl->queue_mutex);
    size_t count = sub_impl->queue_count;
    pthread_mutex_unlock(&sub_impl->queue_mutex);
    return count;
}

// Waits until the subscription holds `count` samples: delivery is in-process, but not assumed to be synchronous.
static void await_queued(rmw_subscription_t* sub, size_t count) {
    for (long waited = 0; queued(sub) < count && waited < ARRIVAL_WAIT_MS; waited++) {
        sleep_ms(1);
    }
    assert(queued(sub) >= count);
}

static bool take(rmw_subscription_t* sub, uint32_t* id) {
    struct pair message = {0, 0};
    bool taken = false;
    rmw_message_info_t info;
    assert(RMW_RET_OK == rmw_take_with_info(sub, &message, &taken, &info, NULL));
    *id = message.first;
    return taken;
}

static rmw_time_t lifespan_of(uint64_t millis) {
    if (0 == millis) {
        return (rmw_time_t)RMW_QOS_LIFESPAN_DEFAULT; // unspecified: infinite
    }
    return (rmw_time_t) {.sec = millis / MS_PER_SEC, .nsec = (millis % MS_PER_SEC) * MS_NS};
}

struct arm {
    const char* name;
    uint64_t writer_ms; // 0: none
    uint64_t reader_ms;
    bool later_taken;
};

static void run_arm(rmw_node_t* node, const struct arm* arm, int index) {
    char topic[TOPIC_BYTES];
    snprintf(topic, sizeof(topic), "/lifespan_writer_%d", index);
    rmw_qos_profile_t writer_qos = rmw_qos_profile_default;
    writer_qos.depth = 8;
    writer_qos.lifespan = lifespan_of(arm->writer_ms);
    rmw_qos_profile_t reader_qos = writer_qos;
    reader_qos.lifespan = lifespan_of(arm->reader_ms);
    rmw_publisher_options_t pub_options = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, &pair_handle, topic, &writer_qos, &pub_options);
    assert(NULL != pub);
    rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &pair_handle, topic, &reader_qos, &sub_options);
    assert(NULL != sub);

    struct pair message = {.first = 1, .second = 0};
    assert(RMW_RET_OK == rmw_publish(pub, &message, NULL));
    await_queued(sub, 1);
    uint32_t id = 0;
    bool at_once = take(sub, &id) && 1 == id;
    message.first = 2;
    assert(RMW_RET_OK == rmw_publish(pub, &message, NULL));
    await_queued(sub, 1);
    sleep_ms(LATER_MS);
    bool later = take(sub, &id);
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    printf("%-20s writer %4llu ms, reader %4llu ms: taken at once %s, %ld ms later %s (writer_lifespans %d)\n",
           arm->name, (unsigned long long)arm->writer_ms, (unsigned long long)arm->reader_ms, at_once ? "yes" : "NO",
           LATER_MS, later ? "taken" : "dropped", sub_impl->tickle_subscriber.writer_lifespans);
    assert(at_once);
    assert(later == arm->later_taken);
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
}

// A sample from `pub`, stamped `source_ns` by its writer's clock, handed to the subscription as core delivers it.
static void deliver_from(rmw_subscription_t* sub, rmw_publisher_t* pub, uint8_t context_id, uint32_t id,
                         uint64_t source_ns) {
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    rmw_tickle_publisher_t* pub_impl = (rmw_tickle_publisher_t*)pub->data;
    uint8_t wire[RMW_TICKLE_PSN_BYTES + sizeof(struct pair)] = {0};
    uint32_t header = rmw_tickle_psn_write(id, wire);
    struct pair message = {.first = id, .second = 0};
    memcpy(wire + header, &message, sizeof(message));
    struct tt_Data* data = sub_impl->topic.data_decode_inplace(wire, header + (uint32_t)sizeof(message), true);
    assert(NULL != data);
    struct tt_Subscriber* tt_sub = &sub_impl->tickle_subscriber;
    tt_sub->delivering_source = context_id; // as core sets them for the callback
    tt_sub->delivering_entity_id = pub_impl->tickle_publisher.endpoint.entity_id;
    tt_sub->callback(tt_sub, source_ns, 0, data);
}

static void run_offset(rmw_node_t* node, uint8_t context_id) {
    rmw_qos_profile_t writer_qos = rmw_qos_profile_default;
    writer_qos.depth = 8;
    writer_qos.lifespan = lifespan_of(OFFSET_LIFESPAN_MS);
    rmw_qos_profile_t reader_qos = rmw_qos_profile_default;
    reader_qos.depth = 8;
    rmw_publisher_options_t pub_options = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, &pair_handle, "/lifespan_offset", &writer_qos, &pub_options);
    assert(NULL != pub);
    rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub =
        rmw_create_subscription(node, &pair_handle, "/lifespan_offset", &reader_qos, &sub_options);
    assert(NULL != sub);
    const struct {
        const char* name;
        int64_t offset_ms; // the writer's clock minus the reader's
        bool taken;
    } offsets[] = {
        {"same host (offset 0)", 0, true},
        {"writer 400 ms behind", -(int64_t)OFFSET_MS, false},
        {"writer 1 h ahead", (int64_t)AHEAD_MS, true},
    };
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint64_t stamp = (uint64_t)((int64_t)tt_get_wall_ns() + (offsets[i].offset_ms * (int64_t)MS_NS));
        deliver_from(sub, pub, context_id, (uint32_t)(10 + i), stamp);
        sleep_ms(OFFSET_LATER_MS);
        uint32_t id = 0;
        bool taken = take(sub, &id);
        printf("offset: %-22s writer lifespan %llu ms, taken %ld ms after arrival: %s\n", offsets[i].name,
               (unsigned long long)OFFSET_LIFESPAN_MS, OFFSET_LATER_MS, taken ? "taken" : "dropped");
        assert(taken == offsets[i].taken);
        assert(!taken || id == (uint32_t)(10 + i));
    }
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); // each arm's line survives a failed assert() after it
    pair_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* node = rmw_create_node(&context, "test_lifespan_writer", "/");
    assert(NULL != node);

    const struct arm arms[] = {
        {"neither (control)", 0, 0, true},
        {"long both (control)", LONG_MS, LONG_MS, true},
        {"writer only", SHORT_MS, 0, false},
        {"reader only", 0, SHORT_MS, false},
        {"writer shorter", SHORT_MS, LONG_MS, false},
        {"reader shorter", LONG_MS, SHORT_MS, false},
    };
    for (size_t i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
        run_arm(node, &arms[i], (int)i);
    }
    rmw_tickle_context_impl_t* context_impl = (rmw_tickle_context_impl_t*)context.impl;
    run_offset(node, context_impl->tickle_context.id);

    assert(RMW_RET_OK == rmw_destroy_node(node));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    printf("test_lifespan_writer: PASS\n");
    return 0;
}
