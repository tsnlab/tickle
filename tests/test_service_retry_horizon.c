/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A retry that reaches the server after it stopped keeping the answer runs the service callback again (2026-10-21).
//
// A Server keeps an answered response for as long as its clients may still be retrying (server_cache_lifetime()), and
// it can only know that from its own tt_Service: call_retry_interval and call_retry_count, which nothing on the wire
// carries. rmw_tickle's clients retry every 100 ms for 5 s (tt_ClientOptions, rmw_client.c), and until this date
// rmw_service.c left its server's tt_Service at 0, 0 - the auto schedule, 5 + 10 + 20 + 40 = 75 ms. A request whose
// answer was lost was then retried 100 ms later, found nothing kept, and was served a second time: a non-idempotent
// service ran twice for one request, where a ROS service request is served once.
//
// Each side here has its own tt_Service, as in rmw_tickle, and the datagrams are carried by hand (the mock HAL
// captures what each context sends), with the clock and both contexts' timers driven in time order. A CallResponse is
// dropped, and the client's retries are dropped up to the one that is delivered. Counted: callback runs for the one
// request, and answers the client gets.
//
// Arms, each with the reading written into its expectation:
// - control: a client retrying every 50 ms - inside 75 ms - against the 0, 0 server: one run. Shows a retry the
//   server still holds the answer for is counted as no re-run, so a 2 below is the expiry and not the harness.
// - the pre-fix rmw shape, client 100 ms x 50 against a 0, 0 server: two runs. The defect, kept as an arm: it shows
//   the scenario can produce a duplicate, so the fixed arms' 1 is not a scenario that cannot fail.
// - the fixed rmw shape, the server's tt_Service carrying the client's 100 ms x 50: one run, for the first retry
//   delivered, for the last retry before the call gives up (49 of 50 lost), synchronous and deferred
//   (tt_CALL_DEFERRED + tt_Server_send_response(), the way rmw_tickle answers).
// - no loss, fixed shape: one run, one answer, and the normal path's cost - how many scheduler entries the answered
//   response holds - which must stay at one per cached answer (what it was with a 75 ms lifetime).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#define SERVER_ID 1
#define CLIENT_ID 2
#define SERVER_IP 0x0a000001
#define CLIENT_IP 0x0a000002
#define PORT 40000
#define CLOCK_NS (100ULL * tt_SECOND)
#define CAPTURE_MAX 16
#define ANSWER_OFFSET 1000U

// rmw_tickle's client retry (rmw_tickle.h RMW_TICKLE_CLIENT_RETRY_INTERVAL_NS, rmw_client.c): restated here because
// core's tests do not include rmw headers; rmw_tickle/test/test_service_retry_horizon.c checks rmw's own values.
#define RMW_INTERVAL 100000000U // 100 ms
#define RMW_COUNT ((uint32_t)(tt_SERVER_DEFERRED_RESPONSE_TIMEOUT / RMW_INTERVAL))

struct message {
    uint32_t tag;
};

static int32_t message_size(struct message* msg) {
    (void)msg;
    return (int32_t)sizeof(uint32_t);
}
static int32_t message_encode(struct message* msg, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, &msg->tag, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}
static int32_t message_decode(struct message* msg, const uint8_t* payload, const uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(&msg->tag, payload, sizeof(uint32_t));
    return (int32_t)sizeof(uint32_t);
}
static void message_free(struct message* msg) {
    (void)msg;
}

#define ECHO_SERVICE                                                   \
    {                                                                  \
        .name = "retry_horizon::srv::dds_::Echo_",                     \
        .request_size = sizeof(struct message),                        \
        .response_size = sizeof(struct message),                       \
        .request_encode_size = (tt_REQUEST_ENCODE_SIZE)message_size,   \
        .request_encode = (tt_REQUEST_ENCODE)message_encode,           \
        .request_decode = (tt_REQUEST_DECODE)message_decode,           \
        .request_free = (tt_REQUEST_FREE)message_free,                 \
        .response_encode_size = (tt_RESPONSE_ENCODE_SIZE)message_size, \
        .response_encode = (tt_RESPONSE_ENCODE)message_encode,         \
        .response_decode = (tt_RESPONSE_DECODE)message_decode,         \
        .response_free = (tt_RESPONSE_FREE)message_free,               \
    }

// One tt_Service per side, as rmw_tickle has (rmw_service.c, rmw_client.c).
static struct tt_Service server_service = ECHO_SERVICE;
static struct tt_Service client_service = ECHO_SERVICE;

static struct tt_Context server_context;
static struct tt_Context client_context;
static struct tt_Server server;
static struct tt_Client client;

static int runs;    // callback runs for the one request
static int answers; // answers the client got
static int wrong_answers;
static bool deferring;
static bool deferred_pending;
static tt_RequestId deferred_id;

static int8_t serve(struct tt_Server* srv, struct message* request, struct message* response, tt_RequestId request_id) {
    (void)srv;
    runs++;
    if (deferring) {
        deferred_pending = true;
        deferred_id = request_id;
        return tt_CALL_DEFERRED;
    }
    response->tag = request->tag + ANSWER_OFFSET;
    return 0;
}

static void answered(struct tt_Client* answering, int8_t return_code, struct message* response) {
    if (answering != &client || return_code != 0 || response == NULL || response->tag != ANSWER_OFFSET) {
        wrong_answers++;
        return;
    }
    answers++;
}

struct capture {
    uint8_t data[CAPTURE_MAX][tt_MAX_BUFFER_LENGTH * 2];
    size_t length[CAPTURE_MAX];
    int count;
};
static struct capture requests;
static struct capture responses;
static struct capture* capturing;

static void capture(const void* buf, size_t len) {
    if (capturing != NULL && capturing->count < CAPTURE_MAX && len <= sizeof(capturing->data[0])) {
        memcpy(capturing->data[capturing->count], buf, len);
        capturing->length[capturing->count++] = len;
    }
}

static void carry_all(const struct capture* from, struct tt_Context* to, uint32_t ip) {
    for (int i = 0; i < from->count; i++) {
        memcpy(to->rx_buffer, from->data[i], from->length[i]);
        (void)process_packet(to, to->rx_buffer, 0, (uint32_t)from->length[i], ip, PORT, tt_TRANSPORT_UDP);
    }
}

static void init_context(struct tt_Context* context, uint8_t id) {
    memset(context, 0, sizeof(*context));
    node_init_locks(context);
    context->id = id;
    context->tx_tail = sizeof(struct tt_Header);
    context->tx_size = tt_TX_BUFFER_LENGTH;
    context->last_modified = CLOCK_NS;
}

struct arm {
    const char* name;
    uint32_t server_interval; // the server's tt_Service.call_retry_interval / _count
    uint32_t server_count;
    uint32_t client_interval; // the client's tt_ClientOptions
    uint32_t client_count;
    bool deferred;
    bool lose_response; // the first CallResponse is dropped
    uint32_t delivered; // the retry that reaches the server; the ones before it are dropped (0: none sent)
    int expected_runs;  // the reading, pre-registered
    int expected_answers;
};

static void setup(const struct arm* arm) {
    test_mock_reset();
    test_mock_now = CLOCK_NS;
    init_context(&server_context, SERVER_ID);
    init_context(&client_context, CLIENT_ID);
    memset(&server, 0, sizeof(server));
    memset(&client, 0, sizeof(client));
    runs = 0;
    answers = 0;
    wrong_answers = 0;
    deferring = arm->deferred;
    deferred_pending = false;
    server_service.call_retry_interval = arm->server_interval;
    server_service.call_retry_count = arm->server_count;
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_server(tt_Context_default_node(&server_context), &server, &server_service,
                                        "retry_horizon/echo", NULL,
                                        &(struct tt_ServerOptions) {.callback = (tt_SERVER_CALLBACK)serve}, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_client(tt_Context_default_node(&client_context), &client, &client_service,
                                        "retry_horizon/echo", NULL,
                                        &(struct tt_ClientOptions) {.callback = (tt_CLIENT_CALLBACK)answered,
                                                                    .call_retry_interval = arm->client_interval,
                                                                    .call_retry_count = arm->client_count},
                                        NULL, 0));
    test_mock_send_hook = capture;
}

// Runs every entry of `context` due by now.
static void run_due(struct tt_Context* context) {
    bool has_next = false;
    uint64_t next = 0;
    while (run_due_entry(context, test_mock_now, &has_next, &next)) {
    }
}

// The client's next call retry, at its time: first the server's timers due by then (an answer kept until before the
// retry has expired when it arrives), then the retry, whose CallRequest goes to `requests`. False if none is armed.
static bool run_next_retry(void) {
    uint64_t at = UINT64_MAX;
    for (int32_t k = 0; k < client_context.scheduler_tail; k++) {
        const struct tt_TCB* entry = &client_context.scheduler[k];
        if (entry->function == call_retry && entry->time < at) {
            at = entry->time;
        }
    }
    if (at == UINT64_MAX) {
        return false;
    }
    if (at > test_mock_now) {
        test_mock_now = at;
    }
    run_due(&server_context);
    requests.count = 0;
    capturing = &requests;
    run_due(&client_context);
    capturing = NULL;
    return true;
}

// The server takes `requests`; whatever it answers - synchronously, or through tt_Server_send_response() for a
// deferred request - lands in `responses`.
static void serve_requests(void) {
    responses.count = 0;
    capturing = &responses;
    carry_all(&requests, &server_context, CLIENT_IP);
    if (deferred_pending) {
        deferred_pending = false;
        struct message response = {ANSWER_OFFSET};
        EXPECT_EQ_INT(tt_RET_OK, tt_Server_send_response(&server, deferred_id, 0, (struct tt_Response*)&response));
    }
    capturing = NULL;
}

// Scheduler entries the server's answered responses hold.
static int cache_timers(void) {
    int held = 0;
    for (int32_t k = 0; k < server_context.scheduler_tail; k++) {
        held += server_context.scheduler[k].function == server_cache_clean ? 1 : 0;
    }
    return held;
}

static void run_arm(const struct arm* arm) {
    setup(arm);
    requests.count = 0;
    capturing = &requests;
    struct message request = {0};
    EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&client, (struct tt_Request*)&request));
    capturing = NULL;
    EXPECT_EQ_INT(1, requests.count);
    serve_requests();
    EXPECT_EQ_INT(1, responses.count);
    if (!arm->lose_response) {
        carry_all(&responses, &client_context, SERVER_IP);
        EXPECT_EQ_INT(1, cache_timers()); // the normal path: one entry per kept answer, as with a 75 ms lifetime
    }
    for (uint32_t retry = 1; retry <= arm->delivered; retry++) {
        EXPECT_TRUE(run_next_retry());
        EXPECT_EQ_INT(1, requests.count);
        if (retry == arm->delivered) {
            serve_requests();
            carry_all(&responses, &client_context, SERVER_IP);
        }
    }
    printf("  %-58s runs %d (expected %d), answers %d, retry %u at +%llu ms\n", arm->name, runs, arm->expected_runs,
           answers, arm->delivered, (test_mock_now - CLOCK_NS) / tt_MILLISECOND);
    EXPECT_EQ_INT(arm->expected_runs, runs);
    EXPECT_EQ_INT(arm->expected_answers, answers);
    EXPECT_EQ_INT(0, wrong_answers);
    test_mock_send_hook = NULL;
}

// The cost on the normal path: answers kept, and the scheduler entries they hold, under a steady CALLS calls 100 ms
// apart, every one answered. A client's answers are bounded by tt_CLIENT_CALL_WINDOW (set_server_cache()) however long
// each is kept, so the 5.1 s lifetime holds at most that many per calling client, where 75 ms held one.
#define CALLS 20
static void test_kept_answers(const char* name, uint32_t server_interval, uint32_t server_count, int expected) {
    const struct arm shape = {name, server_interval, server_count, RMW_INTERVAL, RMW_COUNT, false, false, 0, 1, 1};
    setup(&shape);
    for (uint32_t k = 0; k < CALLS; k++) {
        test_mock_now = CLOCK_NS + (k * (uint64_t)RMW_INTERVAL);
        run_due(&server_context);
        run_due(&client_context);
        requests.count = 0;
        capturing = &requests;
        struct message request = {0};
        EXPECT_EQ_INT(tt_RET_OK, tt_Client_call(&client, (struct tt_Request*)&request));
        capturing = NULL;
        serve_requests();
        carry_all(&responses, &client_context, SERVER_IP);
    }
    printf("  %-58s %d calls, answers %d, kept %d (expected %d)\n", name, CALLS, answers, cache_timers(), expected);
    EXPECT_EQ_INT(CALLS, runs);
    EXPECT_EQ_INT(CALLS, answers);
    EXPECT_EQ_INT(expected, cache_timers());
    test_mock_send_hook = NULL;
}

int main(void) {
    const uint32_t control_interval = 50U * 1000U * 1000U;
    const struct arm arms[] = {
        {"control: client 50 ms vs server 0,0, retry 1", 0, 0, control_interval, 100, false, true, 1, 1, 1},
        {"pre-fix rmw: client 100 ms vs server 0,0, retry 1", 0, 0, RMW_INTERVAL, RMW_COUNT, false, true, 1, 2, 1},
        {"pre-fix rmw, deferred", 0, 0, RMW_INTERVAL, RMW_COUNT, true, true, 1, 2, 1},
        {"fixed rmw: server 100 ms x 50, retry 1", RMW_INTERVAL, RMW_COUNT, RMW_INTERVAL, RMW_COUNT, false, true, 1, 1,
         1},
        {"fixed rmw, deferred, retry 1", RMW_INTERVAL, RMW_COUNT, RMW_INTERVAL, RMW_COUNT, true, true, 1, 1, 1},
        {"fixed rmw, deferred, retries 1-49 lost, retry 50", RMW_INTERVAL, RMW_COUNT, RMW_INTERVAL, RMW_COUNT, true,
         true, RMW_COUNT, 1, 1},
        {"fixed rmw, no loss", RMW_INTERVAL, RMW_COUNT, RMW_INTERVAL, RMW_COUNT, true, false, 0, 1, 1},
    };
    for (size_t i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
        run_arm(&arms[i]);
    }

    test_kept_answers("kept answers, pre-fix rmw (75 ms)", 0, 0, 1);
    test_kept_answers("kept answers, fixed rmw (5.1 s)", RMW_INTERVAL, RMW_COUNT,
                      tt_CLIENT_MAX_OUTSTANDING == 1 ? 1 : tt_CLIENT_CALL_WINDOW);

    if (test_result() != 0) {
        return 1;
    }
    printf("test_service_retry_horizon: all tests passed\n");
    return 0;
}
