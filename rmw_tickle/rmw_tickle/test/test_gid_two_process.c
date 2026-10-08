/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A gid is only useful if the side that receives it can match it to the side that made it, and the two
// sides are two processes. So this test is two processes: a talker (forked child, its own rmw context)
// with two publishers on one topic, one on another and a client, and a listener (this process) with a
// subscription on each topic and the service. The talker sends the gids its own rmw reports -
// rmw_get_gid_for_publisher() and rmw_get_gid_for_client() - down a pipe, and the listener requires that:
//
//   1. every taken sample's message_info.publisher_gid equals the gid its writer's process reports, and
//      rmw_compare_gids_equal() says so; and that it is UNEQUAL to the other writer's gid on that topic
//      (the property that fails if every sample carries one constant gid, all zero or not);
//   2. rmw_get_publishers_info_by_topic() reports that same gid for each remote writer, so a tool can match a
//      sample to an endpoint it discovered through the graph - both writers of the shared topic, a row each
//      (graph_matches());
//   3. a taken request's request_id.writer_guid equals the talker's client gid (or is all zeros, for a request
//      that beat the client's announce - never anything else), the response's names the same client, and
//      rmw_get_clients_info_by_service() reports that gid too (lyrical and later; jazzy has no such call).
//
// History: until 2026-10-02 every sample carried sixteen zero bytes, until 4d97f90a the graph encoded a
// topic-name hash instead of the writer's entity id, and until 2026-10-08 a request's writer_guid was zero
// on both sides.

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
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/topic_endpoint_info_array.h"
#include "rmw/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"
#include "rosidl_typesupport_tickle_c/service_type_support.h"

// *_info_by_service is lyrical's; CI builds against jazzy, which has no such entry points (test_g3_events.c).
#if defined(__has_include)
#if __has_include("rmw/get_service_endpoint_info.h")
#include "rmw/get_service_endpoint_info.h"
#include "rmw/service_endpoint_info.h"
#include "rmw/service_endpoint_info_array.h"
#define HAVE_SERVICE_ENDPOINT_INFO 1
#endif
#endif

#define TOPIC "/gid_two_process"
#define SOLO_TOPIC "/gid_two_process_solo" // one writer: see graph_matches()
#define SERVICE "/gid_two_process_srv"
#define WRITERS 2
#define WANT_PER_WRITER 3 // samples per writer the listener must match before it is satisfied
#define TALKER_TICKS 600  // POLL_MS apart: 30 s, after which the talker gives up, so a dead listener cannot strand it
#define REQUEST_EVERY 20  // ticks: one request at a time is all a client holds, and 1 s answers or times out one
#define LISTENER_POLLS 400
#define POLL_MS 50L
#define NS_PER_MS 1000000L

struct pair {
    uint32_t first;  // which writer: 0 or 1 (a request: always 0)
    uint32_t second; // a running count
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

#define PAIR_CALLBACKS(type_name)                                                    \
    {                                                                                \
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
    }

static rosidl_typesupport_tickle_c_message_callbacks_t message_callbacks = PAIR_CALLBACKS("gid_two_process/msg/Pair");
static rosidl_message_type_support_t message_handle = {.data = &message_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_message_callbacks_t request_callbacks =
    PAIR_CALLBACKS("gid_two_process/srv/Echo_Request");
static rosidl_typesupport_tickle_c_message_callbacks_t response_callbacks =
    PAIR_CALLBACKS("gid_two_process/srv/Echo_Response");
static rosidl_message_type_support_t request_handle = {.data = &request_callbacks,
                                                       .func = get_message_typesupport_handle_function};
static rosidl_message_type_support_t response_handle = {.data = &response_callbacks,
                                                        .func = get_message_typesupport_handle_function};
static rosidl_typesupport_tickle_c_service_callbacks_t service_callbacks = {.ros_type_name =
                                                                                "gid_two_process/srv/Echo"};
static rosidl_service_type_support_t service_handle = {.data = &service_callbacks,
                                                       .func = get_service_typesupport_handle_function,
                                                       .request_typesupport = &request_handle,
                                                       .response_typesupport = &response_handle};

// What the talker tells the listener: the gids its own rmw reports for what it created.
struct talker_gids {
    uint8_t writer[WRITERS][RMW_GID_STORAGE_SIZE];
    uint8_t solo[RMW_GID_STORAGE_SIZE];
    uint8_t client[RMW_GID_STORAGE_SIZE];
};

static void sleep_poll(void) {
    struct timespec interval = {.tv_sec = 0, .tv_nsec = POLL_MS * NS_PER_MS};
    nanosleep(&interval, NULL);
}

static void print_gid(const char* label, const uint8_t* gid) {
    printf("%s ", label);
    for (unsigned i = 0; i < RMW_GID_STORAGE_SIZE; i++) {
        printf("%02x", gid[i]);
    }
    printf("\n");
}

static bool is_zero(const uint8_t* gid) {
    static const uint8_t zero[RMW_GID_STORAGE_SIZE] = {0};
    return 0 == memcmp(gid, zero, RMW_GID_STORAGE_SIZE);
}

static rmw_gid_t as_gid(const uint8_t* data) {
    rmw_gid_t gid;
    memset(&gid, 0, sizeof(gid));
    gid.implementation_identifier = rmw_get_implementation_identifier();
    memcpy(gid.data, data, RMW_GID_STORAGE_SIZE);
    return gid;
}

static bool gids_equal(const uint8_t* lhs, const uint8_t* rhs) {
    rmw_gid_t first = as_gid(lhs);
    rmw_gid_t second = as_gid(rhs);
    bool result = false;
    assert(RMW_RET_OK == rmw_compare_gids_equal(&first, &second, &result));
    return result;
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

// The child. Publishes from both writers and keeps a request outstanding until the listener closes
// `quit_fd`, or TALKER_SECONDS pass.
static int run_talker(int gids_fd, int quit_fd) {
    rmw_init_options_t options;
    rmw_context_t context;
    init_context(&options, &context);
    rmw_node_t* node = rmw_create_node(&context, "gid_talker", "/");
    assert(NULL != node);
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    rmw_publisher_options_t pub_options = rmw_get_default_publisher_options();
    rmw_publisher_t* writers[WRITERS];
    struct talker_gids gids;
    for (int which = 0; which < WRITERS; which++) {
        writers[which] = rmw_create_publisher(node, &message_handle, TOPIC, &qos, &pub_options);
        assert(NULL != writers[which]);
        rmw_gid_t gid;
        assert(RMW_RET_OK == rmw_get_gid_for_publisher(writers[which], &gid));
        memcpy(gids.writer[which], gid.data, RMW_GID_STORAGE_SIZE);
    }
    rmw_publisher_t* solo = rmw_create_publisher(node, &message_handle, SOLO_TOPIC, &qos, &pub_options);
    assert(NULL != solo);
    rmw_gid_t solo_gid;
    assert(RMW_RET_OK == rmw_get_gid_for_publisher(solo, &solo_gid));
    memcpy(gids.solo, solo_gid.data, RMW_GID_STORAGE_SIZE);
    rmw_qos_profile_t service_qos = rmw_qos_profile_services_default;
    rmw_client_t* client = rmw_create_client(node, &service_handle, SERVICE, &service_qos);
    assert(NULL != client);
    rmw_gid_t client_gid;
    assert(RMW_RET_OK == rmw_get_gid_for_client(client, &client_gid));
    memcpy(gids.client, client_gid.data, RMW_GID_STORAGE_SIZE);
    assert((ssize_t)sizeof(gids) == write(gids_fd, &gids, sizeof(gids)));
    close(gids_fd);

    assert(0 == fcntl(quit_fd, F_SETFL, O_NONBLOCK));
    uint32_t count = 0;
    int64_t sequence = 0;
    for (int tick = 0; tick < TALKER_TICKS; tick++) {
        char byte = 0;
        if (0 == read(quit_fd, &byte, 1)) {
            break; // end of file: the listener closed its end, it has what it needs
        }
        for (uint32_t which = 0; which < WRITERS; which++) {
            struct pair message = {.first = which, .second = count};
            assert(RMW_RET_OK == rmw_publish(writers[which], &message, NULL));
        }
        struct pair solo_message = {.first = 0, .second = count};
        assert(RMW_RET_OK == rmw_publish(solo, &solo_message, NULL));
        if (0 == count % REQUEST_EVERY) {
            struct pair request = {.first = 0, .second = count};
            assert(RMW_RET_OK == rmw_send_request(client, &request, &sequence));
        }
        struct pair response;
        rmw_service_info_t info;
        bool taken = false;
        assert(RMW_RET_OK == rmw_take_response(client, &info, &response, &taken));
        if (taken) {
            // The response names this client's own request: its writer_guid is this client's gid.
            assert(0 == memcmp(info.request_id.writer_guid, gids.client, RMW_GID_STORAGE_SIZE));
        }
        count++;
        sleep_poll();
    }
    close(quit_fd);
    assert(RMW_RET_OK == rmw_destroy_client(node, client));
    assert(RMW_RET_OK == rmw_destroy_publisher(node, solo));
    for (int which = 0; which < WRITERS; which++) {
        assert(RMW_RET_OK == rmw_destroy_publisher(node, writers[which]));
    }
    assert(RMW_RET_OK == rmw_destroy_node(node));
    fini_context(&options, &context);
    return 0;
}

// The gids the graph reports for a topic's publishers, at most `max`; how many.
static size_t graph_writer_gids(rmw_node_t* node, const char* topic, uint8_t out[][RMW_GID_STORAGE_SIZE], size_t max) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_topic_endpoint_info_array_t infos = rmw_get_zero_initialized_topic_endpoint_info_array();
    assert(RMW_RET_OK == rmw_get_publishers_info_by_topic(node, &allocator, topic, false, &infos));
    size_t count = infos.size;
    for (size_t i = 0; i < count && i < max; i++) {
        memcpy(out[i], infos.info_array[i].endpoint_gid, RMW_GID_STORAGE_SIZE);
    }
    assert(RMW_RET_OK == rmw_topic_endpoint_info_array_fini(&infos, &allocator));
    return count;
}

// The graph's gids for the talker's endpoints are the ones the talker reports, so a sample or a request can be
// matched to an endpoint discovered through the graph. False while discovery has not shown them yet (samples
// can arrive before the announce: a writer is claimed from its first ACKNACK).
//
// SOLO_TOPIC has one writer and one graph row. TOPIC's two writers come from one remote context and share one
// endpoint_id (the topic-name hash); the graph must list both, each under its own gid. Until 2026-10-08 core's
// discovery table keyed an entity on (context_id, endpoint_id), so the second overwrote the first and the graph
// listed one of them; it keys on the entity_id now (upsert_discovered_entity()).
static bool graph_matches(rmw_node_t* node, const struct talker_gids* gids) {
    uint8_t solo[2][RMW_GID_STORAGE_SIZE];
    size_t n_solo = graph_writer_gids(node, SOLO_TOPIC, solo, 2);
    assert(n_solo <= 1);
    uint8_t pair[WRITERS + 1][RMW_GID_STORAGE_SIZE];
    size_t n_pair = graph_writer_gids(node, TOPIC, pair, WRITERS + 1);
    assert(n_pair <= WRITERS); // never more endpoints than exist
    bool complete = 1 == n_solo && WRITERS == n_pair;
    if (complete) {
        printf("graph: %zu publisher on %s, %zu on %s (%d exist)\n", n_solo, SOLO_TOPIC, n_pair, TOPIC, WRITERS);
        print_gid("  graph solo publisher gid:", solo[0]);
        assert(gids_equal(solo[0], gids->solo));
        // Both writers, each once: a row per writer, and the two rows name the two gids the talker reported.
        bool listed[WRITERS] = {false, false};
        for (size_t i = 0; i < n_pair; i++) {
            print_gid("  graph pair publisher gid:", pair[i]);
            int which = gids_equal(pair[i], gids->writer[0]) ? 0 : 1;
            assert(gids_equal(pair[i], gids->writer[which]));
            assert(!listed[which]);
            listed[which] = true;
        }
        assert(listed[0] && listed[1]);
    }
#ifdef HAVE_SERVICE_ENDPOINT_INFO
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_service_endpoint_info_array_t clients = rmw_get_zero_initialized_service_endpoint_info_array();
    assert(RMW_RET_OK == rmw_get_clients_info_by_service(node, &allocator, SERVICE, false, &clients));
    assert(clients.size <= 1);
    if (complete && 1 == clients.size) {
        assert(1 == clients.info_array[0].endpoint_count);
        print_gid("  graph client gid:        ", clients.info_array[0].endpoint_gids[0]);
        assert(0 == memcmp(clients.info_array[0].endpoint_gids[0], gids->client, RMW_GID_STORAGE_SIZE));
    }
    complete = complete && 1 == clients.size;
    assert(RMW_RET_OK == rmw_service_endpoint_info_array_fini(&clients, &allocator));
#endif
    return complete;
}

// Takes every queued sample of TOPIC: each must name its own writer and not the other one - the general
// property first. Counts them per writer.
static void take_pair_samples(rmw_subscription_t* sub, const struct talker_gids* gids, int matched[WRITERS]) {
    bool taken = true;
    while (taken) {
        struct pair message;
        rmw_message_info_t info = rmw_get_zero_initialized_message_info();
        assert(RMW_RET_OK == rmw_take_with_info(sub, &message, &taken, &info, NULL));
        if (!taken) {
            break;
        }
        assert(message.first < WRITERS);
        if (0 == matched[message.first]) {
            print_gid(message.first == 0 ? "sample from writer 0:" : "sample from writer 1:", info.publisher_gid.data);
        }
        assert(gids_equal(info.publisher_gid.data, gids->writer[message.first]));
        assert(!gids_equal(info.publisher_gid.data, gids->writer[1 - message.first]));
        matched[message.first]++;
    }
}

// Takes every queued sample of SOLO_TOPIC, each of which must name the solo writer; how many.
static int take_solo_samples(rmw_subscription_t* sub, const struct talker_gids* gids) {
    int count = 0;
    bool taken = true;
    while (taken) {
        struct pair message;
        rmw_message_info_t info = rmw_get_zero_initialized_message_info();
        assert(RMW_RET_OK == rmw_take_with_info(sub, &message, &taken, &info, NULL));
        if (taken) {
            assert(gids_equal(info.publisher_gid.data, gids->solo));
            count++;
        }
    }
    return count;
}

// Answers a pending request, if there is one; true when its writer_guid named the talker's client exactly.
// A request names its source context and not its client, so the client is looked up in discovery
// (rmw_service.c, requester_gid_locked()). A request that arrives before the client's announce cannot be
// attributed and says so with zeros; it must never name anything else.
static bool answer_request(rmw_service_t* service, const struct talker_gids* gids) {
    bool taken = false;
    struct pair request;
    rmw_service_info_t info;
    assert(RMW_RET_OK == rmw_take_request(service, &info, &request, &taken));
    if (!taken) {
        return false;
    }
    print_gid("request writer_guid: ", info.request_id.writer_guid);
    bool exact = 0 == memcmp(info.request_id.writer_guid, gids->client, RMW_GID_STORAGE_SIZE);
    assert(exact || is_zero(info.request_id.writer_guid));
    struct pair response = request;
    assert(RMW_RET_OK == rmw_send_response(service, &info.request_id, &response));
    return exact;
}

int main(void) {
    message_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    request_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    response_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    service_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;

    setvbuf(stdout, NULL, _IONBF, 0); // an assert aborts: what was printed before it must not be lost
    int gids_pipe[2];
    int quit_pipe[2];
    assert(0 == pipe(gids_pipe) && 0 == pipe(quit_pipe));
    fflush(stdout);
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
    rmw_node_t* node = rmw_create_node(&context, "gid_listener", "/");
    assert(NULL != node);
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &message_handle, TOPIC, &qos, &sub_options);
    rmw_subscription_t* solo_sub = rmw_create_subscription(node, &message_handle, SOLO_TOPIC, &qos, &sub_options);
    assert(NULL != sub && NULL != solo_sub);
    rmw_qos_profile_t service_qos = rmw_qos_profile_services_default;
    rmw_service_t* service = rmw_create_service(node, &service_handle, SERVICE, &service_qos);
    assert(NULL != service);

    struct talker_gids gids;
    assert((ssize_t)sizeof(gids) == read(gids_pipe[0], &gids, sizeof(gids)));
    close(gids_pipe[0]);
    print_gid("talker writer 0 gid:", gids.writer[0]);
    print_gid("talker writer 1 gid:", gids.writer[1]);
    print_gid("talker client gid:  ", gids.client);
    // Distinct and non-zero, or nothing below can tell a right answer from a wrong one.
    print_gid("talker solo gid:    ", gids.solo);
    assert(!is_zero(gids.writer[0]) && !is_zero(gids.writer[1]) && !is_zero(gids.solo) && !is_zero(gids.client));
    assert(!gids_equal(gids.writer[0], gids.writer[1]));

    int matched[WRITERS] = {0, 0};
    int solo_matched = 0;
    bool request_checked = false;
    bool graph_checked = false;
    for (int attempt = 0; attempt < LISTENER_POLLS; attempt++) {
        take_pair_samples(sub, &gids, matched);
        solo_matched += take_solo_samples(solo_sub, &gids);
        request_checked = answer_request(service, &gids) || request_checked;
        if (!graph_checked) {
            graph_checked = graph_matches(node, &gids);
        }
        if (matched[0] >= WANT_PER_WRITER && matched[1] >= WANT_PER_WRITER && solo_matched >= WANT_PER_WRITER &&
            request_checked && graph_checked) {
            break;
        }
        sleep_poll();
    }
    printf("matched %d, %d and %d samples to their writers; request checked %d, graph checked %d\n", matched[0],
           matched[1], solo_matched, request_checked, graph_checked);
    assert(matched[0] >= WANT_PER_WRITER && matched[1] >= WANT_PER_WRITER && solo_matched >= WANT_PER_WRITER);
    assert(request_checked && graph_checked);

    close(quit_pipe[1]); // the talker reads end of file and stops
    int status = 0;
    assert(talker == waitpid(talker, &status, 0));
    assert(WIFEXITED(status) && 0 == WEXITSTATUS(status));

    assert(RMW_RET_OK == rmw_destroy_service(node, service));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, solo_sub));
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    fini_context(&options, &context);
    printf("test_gid_two_process: PASS\n");
    return 0;
}
