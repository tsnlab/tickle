/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=perf_server: see main_perf_client.c's file-level comment for why this exists, why it
// reuses examples/linux/perf/Bulk.{c,h} but not perf_server.c itself, and why it reports once a
// second (mirroring perf_server.c's own report()) instead of logging every receive.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "../../format.h"
#include "Bulk.h"
#include "board/uart.h"
#include "net_init.h"

#define PERF_SERVER_TASK_STACK_WORDS 1024

// 5s warm-up + 60s of real measured throughput + 5s cool-down = 70s total, matching
// examples/linux/perf/perf_server.c's own -w/-W/-d defaults exactly - just as compile-time
// constants instead of CLI flags, since there's no argv on a flashed target. TOTAL_DURATION_S
// must match platform/freertos/test.sh's own PERF_DURATION_S for this pair (its QEMU `timeout` is
// what actually stops this task - it runs forever otherwise, see perf_server_task()'s own poll
// loop), or the cool-down tag below won't land on the real last 5 seconds.
#define WARMUP_S 5.0
#define COOLDOWN_S 5.0
#define TOTAL_DURATION_S 70.0

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Node node;
static struct tt_Subscriber sub;

static bool have_first = false;
static uint32_t expected_seq = 0;
static uint64_t first_recv_time = 0; // ns timestamp of the first real message - anchors warm-up,
                                     // not this task's own start_time, the same reasoning
                                     // examples/linux/perf/perf_server.c's own first_recv_time has
                                     // (this role's QEMU instance also starts before perf_client's
                                     // does - see platform/freertos/test.sh's own run_round_trip()).
static uint64_t g_start_time = 0;    // this task's own start - cool-down still anchors to this, not
                                     // first_recv_time, since TOTAL_DURATION_S (and so the actual
                                     // external kill) is timed from process start, not first receive.
static uint64_t total_dropped = 0;
static uint64_t interval_received_msgs = 0;
static uint64_t interval_received_bytes = 0;

static void bulk_callback(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct BulkData* data) {
    (void)subscriber;
    (void)seq_no; // truncated to 16 bits by the framework; data->seq is the real 32-bit one

    if (!have_first) {
        first_recv_time = time;
    }

    // Same drop-detection approach as examples/linux/perf/perf_server.c's own bulk_callback().
    bool gap = have_first && data->seq != expected_seq;
    uint32_t gap_count = gap ? data->seq - expected_seq : 0;
    expected_seq = data->seq + 1;
    have_first = true;

    interval_received_msgs++;
    interval_received_bytes += data->size;

    double elapsed_since_first_s = (double)(time - first_recv_time) / (double)tt_SECOND;
    double elapsed_since_start_s = (double)(time - g_start_time) / (double)tt_SECOND;
    bool in_warmup = elapsed_since_first_s < WARMUP_S;
    bool in_cooldown = elapsed_since_start_s >= TOTAL_DURATION_S - COOLDOWN_S;
    if (!in_warmup && !in_cooldown) {
        total_dropped += gap_count;
    }
}

// Field set and wording mirror examples/linux/perf/perf_server.c's own report(): MB/Mbps computed
// from this interval's bytes, only the cumulative drop count carried across intervals - this is
// the receive side, so (unlike the client) it's the one that can actually see loss.
static void report(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;

    char recv_buf[TT_GROUPED_BUF_LEN];
    char dropped_buf[TT_GROUPED_BUF_LEN];
    char megabytes_buf[TT_GROUPED_F3_BUF_LEN];
    char mbps_buf[TT_GROUPED_F3_BUF_LEN];
    const double bytes_per_mb = 1e6;
    double megabytes = (double)interval_received_bytes / bytes_per_mb;
    double mbps = ((double)interval_received_bytes * 8) / bytes_per_mb;

    double elapsed_since_first_s = have_first ? (double)(time - first_recv_time) / (double)tt_SECOND : 0.0;
    double elapsed_since_start_s = (double)(time - g_start_time) / (double)tt_SECOND;
    bool in_warmup = have_first && elapsed_since_first_s < WARMUP_S;
    bool in_cooldown = elapsed_since_start_s >= TOTAL_DURATION_S - COOLDOWN_S;
    const char* tag = "";
    if (in_warmup) {
        tag = " (warmup)";
    } else if (in_cooldown) {
        tag = " (cooldown)";
    }

    printf("perf_server: recv %s msgs, %s MB, %s Mbps this interval (%s dropped so far)%s\n",
           tt_format_grouped(interval_received_msgs, recv_buf), tt_format_grouped_f3(megabytes, megabytes_buf),
           tt_format_grouped_f3(mbps, mbps_buf), tt_format_grouped(total_dropped, dropped_buf), tag);

    interval_received_msgs = 0;
    interval_received_bytes = 0;

    tt_Node_schedule(node, time + tt_SECOND, report, NULL);
}

static void perf_server_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Node_create(&node);
    if (ret != tt_RET_OK) {
        printf("perf_server: tt_Node_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("perf_server: node created, id=%u\n", node.id);

    ret = tt_Node_create_subscriber(&node, &sub, &BulkTopic, "bulk_topic", (tt_SUBSCRIBER_CALLBACK)bulk_callback);
    if (ret != tt_RET_OK) {
        printf("perf_server: tt_Node_create_subscriber failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("perf_server: ready\n");

    g_start_time = tt_get_ns();
    tt_Node_schedule(&node, g_start_time + tt_SECOND, report, NULL);

    for (;;) {
        tt_Node_poll(&node, -1);
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: perf_server role boot OK\n");

    xTaskCreate(perf_server_task, "perf_server", PERF_SERVER_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}
