/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The introspection round trip, the generalisation of g14 (RMW_GAPS_PLAN.md, `git show 3c0c505b:...`): **any
// value we report through an introspection API must be what the remote side created, and one we would accept
// back.** g14 checked the QoS profile; this file checks the other values, across two processes, because the
// remote half of the graph is served from discovery and the local half from the endpoint table - two code
// paths, and only the remote one had the defects found so far (g14, the gid).
//
// A talker (forked child, its own rmw context) creates a node in a namespace with a publisher on a topic whose
// name is rmw's maximum length, a subscription, a service and a client, and pipes the gids its rmw reports.
// A listener (this process) that creates none of those requires, once discovery shows them:
//
//   names      rmw_get_node_names() lists the talker's node and namespace exactly once, and each is accepted by
//              rmw_validate_node_name() / rmw_validate_namespace(); rmw_get_topic_names_and_types() and
//              rmw_get_service_names_and_types() list each name byte-identical to what the talker created,
//              accepted by rmw_validate_full_topic_name(), with exactly the one type it created - and the
//              reported topic and type are accepted back by rmw_create_publisher() here;
//   by node    the four *_names_and_types_by_node() calls for the remote node give the same names and types;
//   counts     rmw_count_publishers/_subscribers/_services/_clients are 1 for what exists and 0 for the name
//              of the other endpoint kind (the control: a count that ignores the kind cannot pass it);
//   info       rmw_get_publishers_info_by_topic() / _subscriptions_ one row each, with the talker's node name,
//              namespace, type, endpoint type and its own gid;
//   format     rmw_get_serialization_format() is the format rmw_take_serialized_message() hands over: what the
//              remote talker published, taken serialized here, is accepted by rmw_deserialize(), gives back the
//              message, and rmw_serialize() of that message reproduces the bytes.
//
// What it does not check: event counts (test_events.c and test_g3_events.c, in-process), and two writers of one
// topic in one remote context, which the graph lists as one - a core discovery gap (test_gid_two_process.c).

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rcutils/types/rcutils_ret.h"
#include "rcutils/types/string_array.h"
#include "rmw/get_node_info_and_types.h"
#include "rmw/get_service_names_and_types.h"
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/get_topic_names_and_types.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/names_and_types.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/serialized_message.h"
#include "rmw/subscription_options.h"
#include "rmw/topic_endpoint_info.h"
#include "rmw/topic_endpoint_info_array.h"
#include "rmw/types.h"
#include "rmw/validate_full_topic_name.h"
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"
#include "rmw_tickle_c/rmw_tickle.h" // rmw_tickle_subscriber_t, for the subscription's (context id, entity_id)
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

#define TALKER_NODE "intro_talker"
#define TALKER_NAMESPACE "/intro_ns"
#define SUB_TOPIC "/intro_ns/intro_sub"
#define SERVICE "/intro_srv"
#define CLIENT_SERVICE "/intro_cli" // a client with no server, so its name is a client's alone
#define SER_TOPIC "/intro_ser"
#define MESSAGE_TYPE "intro_pkg/msg/Pair"
#define SERVICE_TYPE "intro_pkg/srv/Echo"
#define LONG_TOPIC_LENGTH (RMW_TOPIC_MAX_NAME_LENGTH) // rmw's own maximum, so any prefix on the way would show
#define TALKER_TICKS 600                              // POLL_MS apart: 30 s, then the talker gives up on its own
#define LISTENER_POLLS 300
#define POLL_MS 50L
#define NS_PER_MS 1000000L
#define PAIR_BYTES 8
#define SHOWN_CHARS 40 // of a name, when printed
#define LETTERS 26

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
    if (len < sizeof(struct pair)) {
        return -1;
    }
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
// The direct codec, which the serialized entry points use (test_serialized.c): the same eight bytes.
static int32_t direct_encode_size(const void* ros_message) {
    (void)ros_message;
    return PAIR_BYTES;
}
static int32_t direct_encode(const void* ros_message, uint8_t* payload, uint32_t len) {
    if (len < PAIR_BYTES) {
        return -1;
    }
    memcpy(payload, ros_message, PAIR_BYTES);
    return PAIR_BYTES;
}
static int32_t direct_decode(void* ros_message, const uint8_t* payload, uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < PAIR_BYTES) {
        return -1;
    }
    memcpy(ros_message, payload, PAIR_BYTES);
    return PAIR_BYTES;
}

#define PAIR_CALLBACKS(type_name)                                                    \
    {                                                                                \
        .struct_size = sizeof(rosidl_typesupport_tickle_c_message_callbacks_t),      \
        .ros_type_name = (type_name),                                                \
        .tickle_struct_size = sizeof(struct pair),                                   \
        .ros_struct_size = sizeof(struct pair),                                      \
        .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function) & convert,     \
        .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function) & convert, \
        .tickle_encode_size = (tt_DATA_ENCODE_SIZE) & encode_size,                   \
        .tickle_encode = (tt_DATA_ENCODE) & encode,                                  \
        .tickle_decode = (tt_DATA_DECODE) & decode,                                  \
        .tickle_free = (tt_DATA_FREE) & free_nothing,                                \
        .tickle_max_encoded_size = sizeof(struct pair),                              \
        .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,                            \
        .direct_encode_size = &direct_encode_size,                                   \
        .direct_encode = &direct_encode,                                             \
        .direct_decode = &direct_decode,                                             \
    }

static rosidl_typesupport_tickle_c_message_callbacks_t message_callbacks = PAIR_CALLBACKS(MESSAGE_TYPE);
static rosidl_message_type_support_t message_handle = {.data = &message_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_message_callbacks_t request_callbacks = PAIR_CALLBACKS(SERVICE_TYPE "_Request");
static rosidl_typesupport_tickle_c_message_callbacks_t response_callbacks = PAIR_CALLBACKS(SERVICE_TYPE "_Response");
static rosidl_message_type_support_t request_handle = {.data = &request_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t response_handle = {.data = &response_callbacks,
                                                        .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_service_callbacks_t service_callbacks = {.ros_type_name = SERVICE_TYPE};
static rosidl_service_type_support_t service_handle = {.data = &service_callbacks,
                                                       .func = get_service_typesupport_handle_function,
                                                       .request_typesupport = &request_handle,
                                                       .response_typesupport = &response_handle};

static char long_topic[LONG_TOPIC_LENGTH + 1];

struct talker_gids {
    uint8_t publisher[RMW_GID_STORAGE_SIZE];
    uint8_t subscription[RMW_GID_STORAGE_SIZE];
};

static void sleep_poll(void) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = POLL_MS * NS_PER_MS};
    nanosleep(&interval, NULL);
}

static void init_context(rmw_init_options_t* options, rmw_context_t* context) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    *options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(options, allocator));
    options->enclave = rcutils_strdup("/", allocator);
    *context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(options, context));
}

static void fini_context(rmw_init_options_t* options, rmw_context_t* context) {
    assert(RMW_RET_OK == rmw_shutdown(context));
    assert(RMW_RET_OK == rmw_context_fini(context));
    assert(RMW_RET_OK == rmw_init_options_fini(options));
}

// The child: creates everything, reports its gids, publishes until the listener closes `quit_fd`.
static int run_talker(int gids_fd, int quit_fd) {
    rmw_init_options_t options;
    rmw_context_t context;
    init_context(&options, &context);
    rmw_node_t* node = rmw_create_node(&context, TALKER_NODE, TALKER_NAMESPACE);
    assert(NULL != node);
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    rmw_publisher_options_t pub_options = rmw_get_default_publisher_options();
    rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
    rmw_qos_profile_t service_qos = rmw_qos_profile_services_default;
    rmw_publisher_t* pub = rmw_create_publisher(node, &message_handle, long_topic, &qos, &pub_options);
    rmw_publisher_t* ser_pub = rmw_create_publisher(node, &message_handle, SER_TOPIC, &qos, &pub_options);
    rmw_subscription_t* sub = rmw_create_subscription(node, &message_handle, SUB_TOPIC, &qos, &sub_options);
    rmw_service_t* service = rmw_create_service(node, &service_handle, SERVICE, &service_qos);
    rmw_client_t* client = rmw_create_client(node, &service_handle, CLIENT_SERVICE, &service_qos);
    assert(NULL != pub && NULL != ser_pub && NULL != sub && NULL != service && NULL != client);

    struct talker_gids gids;
    rmw_gid_t gid;
    assert(RMW_RET_OK == rmw_get_gid_for_publisher(pub, &gid));
    memcpy(gids.publisher, gid.data, RMW_GID_STORAGE_SIZE);
    // rmw has no gid getter for a subscription. The graph encodes every endpoint as the publisher's gid is
    // encoded, (context id, entity_id), so that pair is read off this side's endpoint for the listener to compare.
    memset(gids.subscription, 0, sizeof(gids.subscription));
    rmw_tickle_subscriber_t* sub_impl = (rmw_tickle_subscriber_t*)sub->data;
    gids.subscription[0] = ((rmw_tickle_node_t*)node->data)->context_impl->tickle_context.id;
    memcpy(&gids.subscription[1], &sub_impl->tickle_subscriber.endpoint.entity_id, sizeof(uint32_t));
    assert((ssize_t)sizeof(gids) == write(gids_fd, &gids, sizeof(gids)));
    close(gids_fd);

    assert(0 == fcntl(quit_fd, F_SETFL, O_NONBLOCK));
    for (uint32_t tick = 0; tick < TALKER_TICKS; tick++) {
        char byte = 0;
        if (0 == read(quit_fd, &byte, 1)) {
            break; // end of file: the listener has what it needs
        }
        struct pair message = {.first = tick, .second = ~tick};
        assert(RMW_RET_OK == rmw_publish(pub, &message, NULL));
        assert(RMW_RET_OK == rmw_publish(ser_pub, &message, NULL));
        sleep_poll();
    }
    close(quit_fd);
    assert(RMW_RET_OK == rmw_destroy_client(node, client));
    assert(RMW_RET_OK == rmw_destroy_service(node, service));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, ser_pub));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    fini_context(&options, &context);
    return 0;
}

static size_t count_of(rmw_ret_t (*counter)(const rmw_node_t*, const char*, size_t*), const rmw_node_t* node,
                       const char* name) {
    size_t count = 0;
    assert(RMW_RET_OK == counter(node, name, &count));
    return count;
}

// Discovery has shown the talker's four endpoints.
static bool talker_discovered(const rmw_node_t* node) {
    return 1 == count_of(rmw_count_publishers, node, long_topic) &&
           1 == count_of(rmw_count_subscribers, node, SUB_TOPIC) && 1 == count_of(rmw_count_services, node, SERVICE) &&
           1 == count_of(rmw_count_clients, node, CLIENT_SERVICE);
}

// `name` appears in `list` exactly once, with exactly the one type `type`. Prints what it compared.
static void expect_one_entry(const rmw_names_and_types_t* list, const char* what, const char* name, const char* type) {
    size_t found = 0;
    for (size_t i = 0; i < list->names.size; i++) {
        if (0 != strcmp(list->names.data[i], name)) {
            continue;
        }
        found++;
        const rcutils_string_array_t* types = &list->types[i];
        printf("  %s: %.*s%s (%zu chars) -> %zu type(s), first %s\n", what, SHOWN_CHARS, name,
               strlen(name) > SHOWN_CHARS ? "..." : "", strlen(name), types->size,
               types->size > 0 ? types->data[0] : "-");
        assert(1 == types->size && 0 == strcmp(types->data[0], type));
    }
    assert(1 == found);
}

static void fini_names_and_types(rmw_names_and_types_t* list) {
    assert(RMW_RET_OK == rmw_names_and_types_fini(list));
}

static void check_node_names(const rmw_node_t* node) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rcutils_string_array_t names = rcutils_get_zero_initialized_string_array();
    rcutils_string_array_t namespaces = rcutils_get_zero_initialized_string_array();
    assert(RMW_RET_OK == rmw_get_node_names(node, &names, &namespaces));
    assert(names.size == namespaces.size);
    size_t found = 0;
    for (size_t i = 0; i < names.size; i++) {
        int name_result = -1;
        int namespace_result = -1;
        size_t index = 0;
        // Every reported name, not only the talker's: whatever we report must be accepted back.
        assert(RMW_RET_OK == rmw_validate_node_name(names.data[i], &name_result, &index));
        assert(RMW_RET_OK == rmw_validate_namespace(namespaces.data[i], &namespace_result, &index));
        assert(RMW_NODE_NAME_VALID == name_result && RMW_NAMESPACE_VALID == namespace_result);
        if (0 == strcmp(names.data[i], TALKER_NODE) && 0 == strcmp(namespaces.data[i], TALKER_NAMESPACE)) {
            found++;
        }
    }
    printf("  node names: %zu reported, talker %s%s listed %zu time(s)\n", names.size, TALKER_NAMESPACE,
           "/" TALKER_NODE, found);
    assert(1 == found);
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&names));
    assert(RCUTILS_RET_OK == rcutils_string_array_fini(&namespaces));
    (void)allocator;
}

static void check_names_and_types(rmw_node_t* node) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_names_and_types_t topics = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK == rmw_get_topic_names_and_types(node, &allocator, false, &topics));
    for (size_t i = 0; i < topics.names.size; i++) {
        int result = -1;
        size_t index = 0;
        assert(RMW_RET_OK == rmw_validate_full_topic_name(topics.names.data[i], &result, &index));
        assert(RMW_TOPIC_VALID == result);
    }
    expect_one_entry(&topics, "topic", long_topic, MESSAGE_TYPE);
    expect_one_entry(&topics, "topic", SUB_TOPIC, MESSAGE_TYPE);
    fini_names_and_types(&topics);

    rmw_names_and_types_t services = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK == rmw_get_service_names_and_types(node, &allocator, &services));
    for (size_t i = 0; i < services.names.size; i++) {
        int result = -1;
        size_t index = 0;
        assert(RMW_RET_OK == rmw_validate_full_topic_name(services.names.data[i], &result, &index));
        assert(RMW_TOPIC_VALID == result);
    }
    expect_one_entry(&services, "service", SERVICE, SERVICE_TYPE);
    fini_names_and_types(&services);

    // By node: the remote node's own view of the same.
    rmw_names_and_types_t by_node = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK ==
           rmw_get_publisher_names_and_types_by_node(node, &allocator, TALKER_NODE, TALKER_NAMESPACE, false, &by_node));
    expect_one_entry(&by_node, "publisher by node", long_topic, MESSAGE_TYPE);
    fini_names_and_types(&by_node);
    by_node = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK == rmw_get_subscriber_names_and_types_by_node(node, &allocator, TALKER_NODE, TALKER_NAMESPACE,
                                                                    false, &by_node));
    expect_one_entry(&by_node, "subscriber by node", SUB_TOPIC, MESSAGE_TYPE);
    fini_names_and_types(&by_node);
    by_node = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK ==
           rmw_get_service_names_and_types_by_node(node, &allocator, TALKER_NODE, TALKER_NAMESPACE, &by_node));
    expect_one_entry(&by_node, "service by node", SERVICE, SERVICE_TYPE);
    fini_names_and_types(&by_node);
    by_node = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK ==
           rmw_get_client_names_and_types_by_node(node, &allocator, TALKER_NODE, TALKER_NAMESPACE, &by_node));
    expect_one_entry(&by_node, "client by node", CLIENT_SERVICE, SERVICE_TYPE);
    fini_names_and_types(&by_node);
}

static void check_counts(const rmw_node_t* node) {
    size_t pubs = count_of(rmw_count_publishers, node, long_topic);
    size_t subs = count_of(rmw_count_subscribers, node, SUB_TOPIC);
    size_t servers = count_of(rmw_count_services, node, SERVICE);
    size_t clients = count_of(rmw_count_clients, node, CLIENT_SERVICE);
    // The control: each name, counted as the other kind.
    size_t pubs_on_sub = count_of(rmw_count_publishers, node, SUB_TOPIC);
    size_t subs_on_pub = count_of(rmw_count_subscribers, node, long_topic);
    size_t servers_on_client = count_of(rmw_count_services, node, CLIENT_SERVICE);
    size_t clients_on_server = count_of(rmw_count_clients, node, SERVICE);
    printf("  counts: publishers %zu, subscribers %zu, services %zu, clients %zu; other kind %zu %zu %zu %zu\n", pubs,
           subs, servers, clients, pubs_on_sub, subs_on_pub, servers_on_client, clients_on_server);
    assert(1 == pubs && 1 == subs && 1 == servers && 1 == clients);
    assert(0 == pubs_on_sub && 0 == subs_on_pub && 0 == servers_on_client && 0 == clients_on_server);
}

static void expect_endpoint_row(const rmw_topic_endpoint_info_array_t* rows, rmw_endpoint_type_t type,
                                const uint8_t* gid) {
    assert(1 == rows->size);
    const rmw_topic_endpoint_info_t* row = &rows->info_array[0];
    printf("  info: node %s ns %s type %s endpoint %d gid %02x%02x%02x%02x%02x...\n", row->node_name,
           row->node_namespace, row->topic_type, (int)row->endpoint_type, row->endpoint_gid[0], row->endpoint_gid[1],
           row->endpoint_gid[2], row->endpoint_gid[3], row->endpoint_gid[4]);
    assert(0 == strcmp(row->node_name, TALKER_NODE) && 0 == strcmp(row->node_namespace, TALKER_NAMESPACE));
    assert(0 == strcmp(row->topic_type, MESSAGE_TYPE) && type == row->endpoint_type);
    assert(0 == memcmp(row->endpoint_gid, gid, RMW_GID_STORAGE_SIZE));
}

static void check_endpoint_info(rmw_node_t* node, const struct talker_gids* gids) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_topic_endpoint_info_array_t rows = rmw_get_zero_initialized_topic_endpoint_info_array();
    assert(RMW_RET_OK == rmw_get_publishers_info_by_topic(node, &allocator, long_topic, false, &rows));
    expect_endpoint_row(&rows, RMW_ENDPOINT_PUBLISHER, gids->publisher);
    assert(RMW_RET_OK == rmw_topic_endpoint_info_array_fini(&rows, &allocator));
    rows = rmw_get_zero_initialized_topic_endpoint_info_array();
    assert(RMW_RET_OK == rmw_get_subscriptions_info_by_topic(node, &allocator, SUB_TOPIC, false, &rows));
    expect_endpoint_row(&rows, RMW_ENDPOINT_SUBSCRIPTION, gids->subscription);
    assert(RMW_RET_OK == rmw_topic_endpoint_info_array_fini(&rows, &allocator));
}

// The reported topic and type, handed back: rmw_create_publisher() accepts them. Last, because a local publisher
// changes the counts above.
static void check_accepted_back(rmw_node_t* node) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_names_and_types_t topics = rmw_get_zero_initialized_names_and_types();
    assert(RMW_RET_OK == rmw_get_topic_names_and_types(node, &allocator, false, &topics));
    for (size_t i = 0; i < topics.names.size; i++) {
        if (0 != strcmp(topics.names.data[i], long_topic)) {
            continue;
        }
        assert(0 == strcmp(topics.types[i].data[0], message_callbacks.ros_type_name));
        rmw_qos_profile_t qos = rmw_qos_profile_default;
        rmw_publisher_options_t options = rmw_get_default_publisher_options();
        rmw_publisher_t* pub = rmw_create_publisher(node, &message_handle, topics.names.data[i], &qos, &options);
        printf("  accepted back: rmw_create_publisher(reported %zu-char name) %s\n", strlen(topics.names.data[i]),
               NULL != pub ? "ok" : "REFUSED");
        assert(NULL != pub);
        assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    }
    fini_names_and_types(&topics);
}

// The serialization format, end to end: a remote sample taken serialized is what rmw_deserialize() accepts
// and rmw_serialize() reproduces. True once one sample has been through it.
static bool check_serialized(rmw_subscription_t* ser_sub) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_serialized_message_t taken_bytes = rmw_get_zero_initialized_serialized_message();
    assert(RCUTILS_RET_OK == rmw_serialized_message_init(&taken_bytes, 0, &allocator));
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_serialized_message(ser_sub, &taken_bytes, &taken, NULL));
    if (taken) {
        struct pair restored = {0, 0};
        assert(RMW_RET_OK == rmw_deserialize(&taken_bytes, &message_handle, &restored));
        assert(restored.second == ~restored.first); // the talker's message, whole
        rmw_serialized_message_t again = rmw_get_zero_initialized_serialized_message();
        assert(RCUTILS_RET_OK == rmw_serialized_message_init(&again, 0, &allocator));
        assert(RMW_RET_OK == rmw_serialize(&restored, &message_handle, &again));
        printf("  format \"%s\": took %zu bytes serialized from the talker, deserialized (%u, %u), reserialized %zu "
               "bytes, %s\n",
               rmw_get_serialization_format(), taken_bytes.buffer_length, restored.first, restored.second,
               again.buffer_length,
               again.buffer_length == taken_bytes.buffer_length &&
                       0 == memcmp(again.buffer, taken_bytes.buffer, again.buffer_length)
                   ? "identical"
                   : "DIFFERENT");
        assert(again.buffer_length == taken_bytes.buffer_length);
        assert(0 == memcmp(again.buffer, taken_bytes.buffer, again.buffer_length));
        assert(RCUTILS_RET_OK == rmw_serialized_message_fini(&again));
    }
    assert(RCUTILS_RET_OK == rmw_serialized_message_fini(&taken_bytes));
    return taken;
}

int main(void) {
    message_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    request_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    response_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    service_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    long_topic[0] = '/';
    for (size_t i = 1; i < LONG_TOPIC_LENGTH; i++) {
        long_topic[i] = (char)('a' + (char)(i % LETTERS));
    }
    long_topic[LONG_TOPIC_LENGTH] = '\0';
    const char* format = rmw_get_serialization_format();
    assert(NULL != format && '\0' != format[0]);

    setvbuf(stdout, NULL, _IONBF, 0); // an assert aborts: what was printed before it must not be lost
    int gids_pipe[2];
    int quit_pipe[2];
    assert(0 == pipe(gids_pipe) && 0 == pipe(quit_pipe));
    pid_t talker = fork();
    assert(talker >= 0);
    if (0 == talker) {
        close(gids_pipe[0]);
        close(quit_pipe[1]);
        _exit(run_talker(gids_pipe[1], quit_pipe[0]));
    }
    close(gids_pipe[1]);
    close(quit_pipe[0]);

    rmw_init_options_t options;
    rmw_context_t context;
    init_context(&options, &context);
    rmw_node_t* node = rmw_create_node(&context, "intro_listener", "/");
    assert(NULL != node);
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
    rmw_subscription_t* ser_sub = rmw_create_subscription(node, &message_handle, SER_TOPIC, &qos, &sub_options);
    assert(NULL != ser_sub);

    struct talker_gids gids;
    assert((ssize_t)sizeof(gids) == read(gids_pipe[0], &gids, sizeof(gids)));
    close(gids_pipe[0]);

    bool discovered = false;
    bool serialized = false;
    for (int attempt = 0; attempt < LISTENER_POLLS && !(discovered && serialized); attempt++) {
        discovered = discovered || talker_discovered(node);
        serialized = serialized || check_serialized(ser_sub);
        sleep_poll();
    }
    printf("discovered %d, serialized sample %d\n", discovered, serialized);
    assert(discovered && serialized);
    check_node_names(node);
    check_names_and_types(node);
    check_counts(node);
    check_endpoint_info(node, &gids);
    check_accepted_back(node);

    close(quit_pipe[1]); // the talker reads end of file and stops
    int status = 0;
    assert(talker == waitpid(talker, &status, 0));
    assert(WIFEXITED(status) && 0 == WEXITSTATUS(status));

    assert(RMW_RET_OK == rmw_destroy_subscription(node, ser_sub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    fini_context(&options, &context);
    printf("test_introspection_two_process: PASS\n");
    return 0;
}
