/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=server: see main_client.c's file-level comment for why this exists and why it reuses
// examples/linux/set_bool/SetBool.{c,h} but not server.c itself.

#include <FreeRTOS.h>
#include <stdio.h>
#include <task.h>

#include <tickle/tickle.h>

#include "SetBool.h"
#include "board/uart.h"
#include "net_init.h"

#define SERVER_TASK_STACK_WORDS 1024

// The answers this server keeps for a retrying client, in storage sized for this example (platform/freertos/Makefile
// builds this role with the inline cache cut to 8 bytes an entry and tt_MAX_SERVER_CACHE_COUNT 8, so the struct
// tt_Server below carries almost nothing of its own; the default is 16 entries of 2,944 bytes - DESIGN.md section 7,
// "Sizing a server's kept answers").
//
// How many answers: a kept answer turns away a retry of a call already run, so the cache must hold every answer whose
// client may still retry. On the auto retry path (SetBoolService's call_retry_* are 0) a client sends nothing past its
// deadline, (tt_CALL_RETRY_COUNT 3 + 1) x tt_CALL_DEADLINE_PER_SEND 250 ms = 1 s after the call, and the server keeps
// an answer for that plus one more 250 ms: L = 1.25 s from when it last went out.
//   - main_client.c calls every T = 500 ms, so with nothing lost ceil(L / T) = ceil(2.5) = 3 answers are live at once.
//   - With loss, a retry served re-arms its answer: the last retry is at most 1 s after the call, so an answer lives
//     at most 1 s + L = 2.25 s, and ceil(2.25 s / 500 ms) = 5 are live. A server that has seen late retries also keeps
//     answers for up to tt_SERVER_CACHE_GAP_MULTIPLE x the longest gap, which can take that higher still.
//   - Whatever the rate or loss, a server keeps at most tt_CLIENT_CALL_WINDOW = 2 x tt_CLIENT_MAX_OUTSTANDING = 8
//     answers per calling Client: past that it replaces the Client's own oldest, whose call the Client's window rule
//     proves finished.
// So 8 entries hold everything one client can still retry, at any call rate. A second client would need 8 more; a
// request that finds every entry live for another client is turned away unexecuted and its retry asks again - never
// run twice (tests/test_service_small_cache.c, both ways).
//
// How large an entry: the CallResponse as sent, 4-byte SubmessageHeader + 8-byte CallResponseHeader + the encoded
// SetBoolResponse, which here is at most 1 (success) + 1 (pad) + 2 (string length) + 6 ("false" and its NUL) = 10
// bytes, padded to 12: 24 bytes. An answer larger than its entry is still sent but not kept, and its retry would run
// the callback again - so MAX_RESPONSE_BYTES must cover every message this callback can answer with.
#define MAX_RESPONSE_BYTES 12U
#define SERVER_ENTRY_BYTES                                                                                   \
    ((sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_CallResponseHeader) + MAX_RESPONSE_BYTES + 7U) & \
     ~(size_t)7U)
_Static_assert(tt_MAX_SERVER_CACHE_COUNT >= 2 * tt_CLIENT_MAX_OUTSTANDING, "every answer one client may retry");

// Too large for a task's own stack - static instead, same reasoning as main.c's ROLE=selftest.
static struct tt_Context node;
static struct tt_Server server;
// 8 x 24 = 192 bytes, uint64_t for the 8-byte alignment the storage needs. tt_Node_create_server() refuses storage
// shorter than tt_Server_storage_size(), so a miscount here fails at start-up, not on a retry.
static uint64_t server_storage[(tt_MAX_SERVER_CACHE_COUNT * SERVER_ENTRY_BYTES) / sizeof(uint64_t)];

static int8_t set_bool_callback(struct tt_Server* server_endpoint, struct SetBoolRequest* request,
                                struct SetBoolResponse* response, tt_RequestId request_id) {
    (void)server_endpoint;
    (void)request_id; // answered synchronously below, no need to defer
    printf("server: request data=%d\n", request->data);

    // message just echoes request->data back as text - "Succeed"/"Failed" read too easily as the
    // *test's* own pass/fail verdict rather than the data value this call carried (see
    // examples/linux/set_bool/server.c's own comment - kept identical here for the same reason).
    if (request->data) {
        response->success = true;
        response->message = "true";
    } else {
        response->success = false;
        response->message = "false";
    }

    return 0;
}

static void server_task(void* param) {
    (void)param;

    net_init();

    tt_ret_t ret = tt_Context_create(&node);
    if (ret != tt_RET_OK) {
        printf("server: tt_Context_create failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("server: node created, id=%u\n", node.id);

    // A NULL profile: the services default (RELIABLE, VOLATILE). The kept answers go in server_storage (see above).
    struct tt_ServerOptions server_options = {.callback = (tt_SERVER_CALLBACK)set_bool_callback,
                                              .max_response_bytes = MAX_RESPONSE_BYTES};
    ret = tt_Node_create_server(tt_Context_default_node(&node), &server, &SetBoolService, "set_bool_server", NULL,
                                &server_options, server_storage, sizeof(server_storage));
    if (ret != tt_RET_OK) {
        printf("server: tt_Node_create_server failed: %d\n", ret);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    printf("server: ready\n");

    for (;;) {
        tt_Context_poll(&node, -1);
    }
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: server role boot OK\n");

    xTaskCreate(server_task, "server", SERVER_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    // vTaskStartScheduler() only returns if it couldn't allocate the idle/timer task - fixed
    // configuration here, so this is unreachable in practice.
    for (;;) {
    }
}
