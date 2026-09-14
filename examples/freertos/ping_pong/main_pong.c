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

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Node node;
static struct tt_Server server;

static int8_t pong_callback(struct tt_Server* server_endpoint, struct PingPongRequest* request,
                            struct PingPongResponse* response) {
    (void)server_endpoint;
    printf("pong: request seq=%lu\n", (unsigned long)request->seq);
    response->seq = request->seq;
    response->timestamp = request->timestamp;
    return 0;
}

static void pong_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Node_create(&node);
    if (ret != tt_RET_OK) {
        printf("pong: tt_Node_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("pong: node created, id=%u\n", node.id);

    ret =
        tt_Node_create_server(&node, &server, &PingPongService, "ping_pong_server", (tt_SERVER_CALLBACK)pong_callback);
    if (ret != tt_RET_OK) {
        printf("pong: tt_Node_create_server failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("pong: server ready\n");

    for (;;) {
        tt_Node_poll(&node, -1);
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
