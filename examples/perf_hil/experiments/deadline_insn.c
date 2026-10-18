/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// QoS plan D4 hot-path check: an rmw pub/sub's work per sample, with and without a DEADLINE set. Run under
// `perf stat -e instructions:u` by deadline_insn.sh, which differences two sample counts so setup and teardown cancel.
// Each sample is one rmw_publish(), rmw_wait() on the subscription, and rmw_take() of exactly that value.
// Prints the loaded librmw_tickle.so's path, so a run against the wrong build shows (AMENT_PREFIX_PATH shadowing).
//
// Usage: deadline_insn <samples> <deadline_ms, 0 = none>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE

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

struct fake_msg {
    uint32_t value;
};

static bool fake_convert(const void* source, void* dest) {
    ((struct fake_msg*)dest)->value = ((const struct fake_msg*)source)->value;
    return true;
}
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    uint32_t value = ((struct fake_msg*)data)->value;
    for (int i = 0; i < 4; i++) {
        payload[i] = (uint8_t)(value >> (8 * i));
    }
    return (int32_t)sizeof(uint32_t);
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= (uint32_t)payload[i] << (8 * i);
    }
    ((struct fake_msg*)data)->value = value;
    return (int32_t)sizeof(uint32_t);
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t fake_callbacks = {
    .ros_type_name = "deadline_insn/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_msg),
    .ros_struct_size = sizeof(struct fake_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_convert,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_convert,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};
static rosidl_message_type_support_t fake_handle = {.data = &fake_callbacks,
                                                    .func = get_message_typesupport_handle_function};

#define NS_PER_MS 1000000U
#define MS_PER_S 1000U
#define MATCH_TRIES 2000
#define WAITS_PER_SAMPLE 100
#define WAIT_MS 10U
#define MAPS_LINE 4096

// Publishes `value` and waits for the subscription to take it. False if it never arrived.
static bool one_sample(rmw_publisher_t* pub, rmw_subscription_t* sub, rmw_wait_set_t* wait_set, uint32_t value) {
    struct fake_msg message = {.value = value};
    if (RMW_RET_OK != rmw_publish(pub, &message, NULL)) {
        return false;
    }
    for (int attempt = 0; attempt < WAITS_PER_SAMPLE; attempt++) {
        void* subs[1] = {sub->data};
        rmw_subscriptions_t subscriptions = {.subscriber_count = 1, .subscribers = subs};
        rmw_time_t timeout = {.sec = 0, .nsec = (uint64_t)WAIT_MS * NS_PER_MS};
        rmw_ret_t waited = rmw_wait(&subscriptions, NULL, NULL, NULL, NULL, wait_set, &timeout);
        (void)waited; // a timeout just means another round; the take decides
        struct fake_msg incoming = {0};
        bool taken = false;
        assert(RMW_RET_OK == rmw_take(sub, &incoming, &taken, NULL));
        if (taken && incoming.value == value) {
            return true;
        }
    }
    return false;
}

// The librmw_tickle.so this process mapped, as the dynamic linker resolved it.
static void print_loaded_library(void) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (NULL == maps) {
        return;
    }
    char line[MAPS_LINE];
    while (NULL != fgets(line, sizeof(line), maps)) {
        const char* path = strchr(line, '/');
        if (NULL != path && NULL != strstr(path, "librmw_tickle.so")) {
            printf("deadline_insn: librmw_tickle=%s", path); // the line ends in its own newline
            break;
        }
    }
    (void)fclose(maps);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        (void)fprintf(stderr, "usage: %s <samples> <deadline_ms>\n", argv[0]);
        return 2;
    }
    uint32_t samples = (uint32_t)strtoul(argv[1], NULL, 10);
    uint64_t deadline_ms = strtoull(argv[2], NULL, 10);
    print_loaded_library();

    fake_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    rmw_node_t* talker = rmw_create_node(&context, "deadline_insn_talker", "/");
    rmw_node_t* listener = rmw_create_node(&context, "deadline_insn_listener", "/");
    assert(NULL != talker && NULL != listener);

    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.depth = 10;
    qos.deadline.sec = deadline_ms / MS_PER_S;
    qos.deadline.nsec = (deadline_ms % MS_PER_S) * NS_PER_MS;
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();
    rmw_subscription_t* sub = rmw_create_subscription(listener, &fake_handle, "/deadline_insn", &qos, &sub_opts);
    rmw_publisher_t* pub = rmw_create_publisher(talker, &fake_handle, "/deadline_insn", &qos, &pub_opts);
    assert(NULL != sub && NULL != pub);
    rmw_wait_set_t* wait_set = rmw_create_wait_set(&context, 1);
    assert(NULL != wait_set);

    // A publish before the match sends nothing, so samples are counted from the first one that arrives.
    uint32_t value = 1;
    int tries = 0;
    while (!one_sample(pub, sub, wait_set, value++)) {
        if (++tries > MATCH_TRIES) {
            printf("deadline_insn: never matched\n");
            return 1;
        }
    }
    uint32_t delivered = 0;
    for (uint32_t i = 0; i < samples; i++) {
        delivered += one_sample(pub, sub, wait_set, value++) ? 1U : 0U;
    }
    printf("deadline_insn: samples=%u delivered=%u deadline_ms=%llu match_tries=%d\n", samples, delivered,
           (unsigned long long)deadline_ms, tries);

    assert(RMW_RET_OK == rmw_destroy_wait_set(wait_set));
    assert(RMW_RET_OK == rmw_destroy_publisher(talker, pub));
    assert(RMW_RET_OK == rmw_destroy_subscription(listener, sub));
    assert(RMW_RET_OK == rmw_destroy_node(talker));
    assert(RMW_RET_OK == rmw_destroy_node(listener));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));
    return delivered == samples ? 0 : 1;
}
