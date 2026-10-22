/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=pong: milestone 4's server side - see main_ping.c's file-level comment for why this
// reuses examples/linux/ping_pong/PingPong.{c,h} but not pong.c itself.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "PingPong.h"
#include "board/uart.h"
#include "net_init.h"

#define PONG_TASK_STACK_WORDS 1024

// The answers this server keeps for a retrying client, in storage sized for this example - the same arithmetic as
// examples/freertos/set_bool/main_server.c, which spells it out: main_ping.c calls every 500 ms on the auto retry path
// (an answer kept L = 1.25 s), so 3 answers are live with nothing lost and 5 with retries re-arming them, and a server
// keeps at most tt_CLIENT_CALL_WINDOW = 8 per calling Client whatever the rate. 8 entries (tt_MAX_SERVER_CACHE_COUNT,
// set for this role by platform/freertos/Makefile with the inline cache cut to 8 bytes an entry), each the
// CallResponse as sent: 4 (SubmessageHeader) + 8 (CallResponseHeader) + 12 (PingPongResponse: seq 4 + timestamp 8) =
// 24 bytes. 192 bytes in all, where the default is 16 x 2,944.
#define MAX_RESPONSE_BYTES 12U
#define SERVER_ENTRY_BYTES                                                                                   \
    ((sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallResponseHeader) + MAX_RESPONSE_BYTES + 7U) & \
     ~(size_t)7U)
_Static_assert(tt_MAX_SERVER_CACHE_COUNT >= 2 * tt_CLIENT_MAX_OUTSTANDING, "every answer one client may retry");

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Context node;
static struct tt_Server server;
static uint64_t server_storage[(tt_MAX_SERVER_CACHE_COUNT * SERVER_ENTRY_BYTES) / sizeof(uint64_t)];

static int8_t pong_callback(struct tt_Server* server_endpoint, struct PingPongRequest* request,
                            struct PingPongResponse* response, tt_RequestId request_id) {
    (void)server_endpoint;
    (void)request_id; // answered synchronously below, no need to defer
    printf("pong: request seq=%lu\n", (unsigned long)request->seq);
    response->seq = request->seq;
    response->timestamp = request->timestamp;
    return 0;
}

static void pong_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Context_create(&node);
    if (ret != tt_RET_OK) {
        printf("pong: tt_Context_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("pong: node created, id=%u\n", node.id);

    // A NULL profile: the services default (RELIABLE, VOLATILE). The kept answers go in server_storage (see above).
    struct tt_ServerOptions server_options = {.callback = (tt_SERVER_CALLBACK)pong_callback,
                                              .max_response_bytes = MAX_RESPONSE_BYTES};
    ret = tt_Node_create_server(tt_Context_default_node(&node), &server, &PingPongService, "ping_pong_server", NULL,
                                &server_options, server_storage, sizeof(server_storage));
    if (ret != tt_RET_OK) {
        printf("pong: tt_Node_create_server failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("pong: server ready\n");

    for (;;) {
        tt_Context_poll(&node, -1);
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: pong role boot OK\n");

    xTaskCreate(pong_task, "pong", PONG_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}
