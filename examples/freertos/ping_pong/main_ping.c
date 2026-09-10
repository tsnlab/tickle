/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=ping: milestone 4's client side - the actual two-instance round trip the whole plan
// (/home/semih/.claude/plans/lively-sauteeing-rainbow.md) has been building up to. This reuses
// examples/linux/ping_pong/PingPong.{c,h} verbatim (pure protocol encode/decode code, no POSIX
// dependency) rather than examples/linux/ping_pong/ping.c itself, which is built around argv
// parsing, SIGINT, and other things that don't exist on this target - the actual tt_Client_call()/
// callback pattern below is otherwise the same one ping.c uses.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "PingPong.h"
#include "board/uart.h"
#include "net_init.h"

#define PING_INTERVAL_NS (500LL * tt_MILLISECOND)
#define PING_TASK_STACK_WORDS 1024

// 5s warm-up + 60s of real measured pinging + 5s cool-down = 70s total, matching
// examples/linux/ping_pong/ping.c's own -w/-W/-d defaults exactly (see its own comment on why -W
// extends past the stop trigger there) - just as compile-time constants instead of CLI flags,
// since there's no argv on a flashed target. TOTAL_DURATION_S must match
// platform/freertos/test.sh's own DURATION_S for the ping/pong pair (its QEMU `timeout` is what
// actually stops this task - it runs forever otherwise, see ping_task()'s own poll loop), or the
// cool-down tag below won't land on the real last 5 seconds.
#define WARMUP_S 5.0
#define COOLDOWN_S 5.0
#define TOTAL_DURATION_S 70.0

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Node node;
static struct tt_Client client;
static uint32_t next_seq = 0;
static uint64_t g_start_time = 0; // set once in ping_task() - anchors warm-up/cool-down tagging

static void ping_callback(struct tt_Client* client, int8_t return_code, struct PingPongResponse* response) {
    (void)client;

    double elapsed_s = (double)(tt_get_ns() - g_start_time) / (double)tt_SECOND;
    const char* tag = "";
    if (elapsed_s < WARMUP_S) {
        tag = " (warmup)";
    } else if (elapsed_s >= TOTAL_DURATION_S - COOLDOWN_S) {
        tag = " (cooldown)";
    }

    if (return_code == tt_CALL_TIMEOUT) {
        printf("ping: timeout (dropped)%s\n", tag);
        return;
    }
    if (return_code != 0) {
        printf("ping: error, return_code=%d%s\n", return_code, tag);
        return;
    }

    // "time=X.XXX ms" (a double, not an integer us count) matches
    // examples/linux/ping_pong/ping.c's own ping_callback() exactly.
    double rtt_ms = (double)(tt_get_ns() - response->timestamp) / (double)tt_MILLISECOND;
    printf("ping: seq=%lu time=%.3f ms%s\n", (unsigned long)response->seq, rtt_ms, tag);
}

static void send_ping(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Client* client = param;

    struct PingPongRequest request = {.seq = next_seq, .timestamp = tt_get_ns()};
    tt_ret_t ret = tt_Client_call(client, (struct tt_Request*)&request);
    if (ret == tt_RET_OK) {
        next_seq++;
    } else if (ret != tt_RET_ILLEGAL_STATUS) {
        // ILLEGAL_STATUS just means the previous call is still outstanding - expected
        // occasionally if a reply/timeout hasn't resolved yet, not worth logging.
        printf("ping: cannot send: %d\n", ret);
    }

    tt_Node_schedule(node, time + PING_INTERVAL_NS, send_ping, client);
}

static void ping_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Node_create(&node);
    if (ret != tt_RET_OK) {
        printf("ping: tt_Node_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("ping: node created, id=%u\n", node.id);

    ret =
        tt_Node_create_client(&node, &client, &PingPongService, "ping_pong_server", (tt_CLIENT_CALLBACK)ping_callback);
    if (ret != tt_RET_OK) {
        printf("ping: tt_Node_create_client failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    g_start_time = tt_get_ns();
    tt_Node_schedule(&node, g_start_time, send_ping, &client);

    for (;;) {
        tt_Node_poll(&node, -1);
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: ping role boot OK\n");

    xTaskCreate(ping_task, "ping", PING_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}
