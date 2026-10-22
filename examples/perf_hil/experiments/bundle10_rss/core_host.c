/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// bundle10_rss: one role of a scenario on the Linux core build, in a process that hosts the context and reads its own
// tables' residency (rss_tables.c) after each step. The context, the Server and the Client are static - .bss, demand
// zero like the calloc() rmw_tickle uses, and like a stack frame nothing has written - so a page of them is resident
// only once something writes it. The publisher's and subscriber's storage is calloc()ed as the examples do.
//
//   core_host LAYOUT ROLE CONTEXT_ID BROADCAST RATE_HZ DUR_S
//     ROLE idle | pub | sub | server | client; the message is UInt64 (8 B), the service SetBool, QoS the default
//     (RELIABLE KEEP_LAST 10). Prints RT/RT_PROC lines (rss_tables.c) labelled "created" (after the context and its
//     endpoint exist) and "end" (after DUR_S of traffic), then "CORE_HOST role=.. sent=.. recv=..".
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>

#include "SetBool.h"
#include "UInt64.h"
#include "rss_tables.c"

static _Alignas(4096) struct tt_Context g_ctx;
static _Alignas(4096) struct tt_Server g_server;
static _Alignas(4096) struct tt_Client g_client;
static struct tt_Publisher g_pub;
static struct tt_Subscriber g_sub;

static volatile int g_done;
static uint64_t g_sent;
static uint64_t g_recv;
static uint64_t g_interval_ns;

static void on_done(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_done = 1;
}

static void publish(struct tt_Context* node, uint64_t time, void* param) {
    (void)param;
    struct UInt64Data data = {.data = g_sent};
    if (tt_Publisher_publish(&g_pub, (struct tt_Data*)&data) == tt_RET_OK) {
        g_sent++;
    }
    tt_Context_schedule(node, time + g_interval_ns, publish, NULL);
}

static void on_data(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct UInt64Data* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    (void)data;
    g_recv++;
}

static int8_t on_request(struct tt_Server* server, struct SetBoolRequest* request, struct SetBoolResponse* response,
                         tt_RequestId request_id) {
    (void)server;
    (void)request_id;
    response->success = request->data;
    response->message = "ok";
    g_recv++;
    return 0;
}

static void on_response(struct tt_Client* client, int8_t return_code, struct SetBoolResponse* response) {
    (void)client;
    (void)response;
    if (return_code == 0) {
        g_recv++;
    }
}

static void call(struct tt_Context* node, uint64_t time, void* param) {
    (void)param;
    struct SetBoolRequest request = {.data = (g_sent & 1U) != 0};
    if (tt_Client_call(&g_client, (struct tt_Request*)&request) == tt_RET_OK) {
        g_sent++;
    }
    tt_Context_schedule(node, time + g_interval_ns, call, NULL);
}

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: core_host LAYOUT idle|pub|sub|server|client CONTEXT_ID BROADCAST RATE_HZ DUR_S\n");
        return 2;
    }
    if (rt_load_layout(argv[1]) != 0) {
        fprintf(stderr, "core_host: cannot read layout %s\n", argv[1]);
        return 2;
    }
    const char* role = argv[2];
    _tt_CONFIG.context_id = atoi(argv[3]);
    _tt_CONFIG.broadcast = argv[4];
    g_interval_ns = (uint64_t)(1e9 / atof(argv[5]));
    const double dur = atof(argv[6]);
    tt_log_set_level(TT_LOG_WARNING);

    if (tt_Context_create(&g_ctx) != tt_RET_OK) {
        fprintf(stderr, "core_host: tt_Context_create failed\n");
        return 1;
    }
    rt_register("tt_Context", &g_ctx);
    struct tt_Node* node = tt_Context_default_node(&g_ctx);
    tt_ret_t ret = tt_RET_OK;
    if (strcmp(role, "pub") == 0) {
        size_t size = tt_Publisher_storage_size(&UInt64Topic, NULL);
        void* storage = calloc(1, size);
        ret = tt_Node_create_publisher(node, &g_pub, &UInt64Topic, "rss_topic", NULL, NULL, storage, size);
        rt_register_range("pub_storage", storage, size);
        rt_register("tt_Publisher", &g_pub);
        tt_Context_schedule(&g_ctx, tt_get_ns() + tt_SECOND, publish, NULL);
    } else if (strcmp(role, "sub") == 0) {
        size_t size = tt_Subscriber_storage_size(&UInt64Topic, NULL);
        void* storage = calloc(1, size);
        struct tt_SubscriberOptions options = {.callback = (tt_SUBSCRIBER_CALLBACK)on_data};
        ret = tt_Node_create_subscriber(node, &g_sub, &UInt64Topic, "rss_topic", NULL, &options, storage, size);
        rt_register_range("sub_storage", storage, size);
        rt_register("tt_Subscriber", &g_sub);
    } else if (strcmp(role, "server") == 0) {
        struct tt_ServerOptions options = {.callback = (tt_SERVER_CALLBACK)on_request};
        ret = tt_Node_create_server(node, &g_server, &SetBoolService, "rss_echo", NULL, &options, NULL, 0);
        rt_register("tt_Server", &g_server);
    } else if (strcmp(role, "client") == 0) {
        struct tt_ClientOptions options = {.callback = (tt_CLIENT_CALLBACK)on_response};
        ret = tt_Node_create_client(node, &g_client, &SetBoolService, "rss_echo", NULL, &options, NULL, 0);
        rt_register("tt_Client", &g_client);
        tt_Context_schedule(&g_ctx, tt_get_ns() + tt_SECOND, call, NULL);
    } else if (strcmp(role, "idle") != 0) {
        fprintf(stderr, "core_host: unknown role %s\n", role);
        return 2;
    }
    if (ret != tt_RET_OK) {
        fprintf(stderr, "core_host: creating the %s failed: %d\n", role, ret);
        return 1;
    }
    rt_snapshot(stdout, "created");
    tt_Context_schedule(&g_ctx, tt_get_ns() + (uint64_t)(dur * 1e9), on_done, NULL);
    while (!g_done) {
        ret = tt_Context_poll(&g_ctx, -1);
        if (ret != tt_RET_OK && ret != tt_RET_TIMEOUT) {
            fprintf(stderr, "core_host: poll returned %d\n", ret);
            break;
        }
    }
    rt_snapshot(stdout, "end");
    printf("CORE_HOST role=%s sent=%llu recv=%llu\n", role, (unsigned long long)g_sent, (unsigned long long)g_recv);
    fflush(stdout);
    tt_Context_destroy(&g_ctx);
    return 0;
}
