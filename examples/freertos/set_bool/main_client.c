/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=client: the set_bool RPC round trip's client side - reuses examples/linux/set_bool/
// SetBool.{c,h} verbatim (pure protocol encode/decode code, no POSIX dependency), same reasoning
// as main_ping.c reusing PingPong.{c,h} over ping.c itself. Logs one line per call outcome
// ("client: call N succeeded/failed") rather than a final PASS/FAIL summary the way
// examples/linux/set_bool/client.c does - this task runs forever (see main_ping.c's own comment
// on why), so there's no "end of run" to summarize; platform/freertos/test.sh instead counts
// these per-call lines the same way it already does for ping/publisher.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "SetBool.h"
#include "board/uart.h"
#include "net_init.h"

#define CALL_INTERVAL_NS (500LL * tt_MILLISECOND)
#define CLIENT_TASK_STACK_WORDS 1024

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Node node;
static struct tt_Client client;
static bool next_data = true;

static void set_bool_callback(struct tt_Client* client, int8_t return_code, struct SetBoolResponse* response) {
    (void)client;

    if (return_code == 0 && response != NULL) {
        printf("client: call succeeded, success=%d\n", response->success);
    } else {
        printf("client: call failed, return_code=%d\n", return_code);
    }
}

static void call(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Client* client = param;

    struct SetBoolRequest request = {.data = next_data};
    tt_ret_t ret = tt_Client_call(client, (struct tt_Request*)&request);
    if (ret == tt_RET_OK) {
        next_data = !next_data;
    } else if (ret != tt_RET_ILLEGAL_STATUS) {
        // ILLEGAL_STATUS just means the previous call is still outstanding - expected
        // occasionally if a reply/timeout hasn't resolved yet, not worth logging.
        printf("client: cannot call: %d\n", ret);
    }

    tt_Node_schedule(node, time + CALL_INTERVAL_NS, call, client);
}

static void client_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Node_create(&node);
    if (ret != tt_RET_OK) {
        printf("client: tt_Node_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("client: node created, id=%u\n", node.id);

    ret = tt_Node_create_client(&node, &client, &SetBoolService, "set_bool_server",
                                (tt_CLIENT_CALLBACK)set_bool_callback);
    if (ret != tt_RET_OK) {
        printf("client: tt_Node_create_client failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    tt_Node_schedule(&node, tt_get_ns(), call, &client);

    for (;;) {
        tt_Node_poll(&node, -1);
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: client role boot OK\n");

    xTaskCreate(client_task, "client", CLIENT_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}
