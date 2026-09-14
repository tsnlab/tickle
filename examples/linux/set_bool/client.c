/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>

#include "../../format.h"
#include "../common/cli_opts.h"
#include "SetBool.h"

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint32_t target_count = 0; // 0 = unlimited
static uint32_t transmitted = 0;
static uint32_t completed = 0; // calls whose outcome (response or error) is known
static uint32_t succeeded = 0; // calls that got a real response with return_code == 0
static uint64_t call_interval_ns = 0;
static bool next_data = true; // alternates the request payload each call, like the original demo
static bool call_data = true; // the data value sent with the currently in-flight call

static void note_completed(void) {
    completed++;
    if (target_count > 0 && completed >= target_count) {
        g_interrupted = 1;
    }
}

// One line per call, printed here (not at send time in call() below) once the outcome is known -
// same shape as ping.c's ping_callback(): a real response is a compact key=value summary,
// timeout/error stay short prose (there's no per-call sequence field in the protocol itself to
// build a "seq=..." line around the way ping_pong's PingPongRequest/Response has one - completed,
// this call's own 0-based index, fills that role instead).
static void set_bool_callback(struct tt_Client* client, int8_t return_code, struct SetBoolResponse* response) {
    (void)client;
    if (return_code == tt_CALL_TIMEOUT) {
        printf("call=%u data=%d no response (timed out)\n", completed, call_data);
    } else if (return_code != 0) {
        printf("call=%u data=%d Error, return_code: %d\n", completed, call_data, return_code);
    } else {
        printf("call=%u data=%d success=%d message=%s\n", completed, call_data, response->success, response->message);
        succeeded++;
    }
    note_completed();
}

// This is the verifying side of the set_bool round trip (see README's "Run examples"): a real
// response - server reachable, request decoded, response decoded back - only ever arrives via
// the return_code==0 branch above, so succeeded==completed is exactly "every call this client
// made got a real answer." transmitted (not completed) is used for the total: a call still
// awaiting a reply when the process is asked to stop counts as neither a pass nor a fail, so
// it's excluded rather than silently counted as one or the other.
static void print_result(void) {
    char transmitted_buf[TT_GROUPED_BUF_LEN];
    char succeeded_buf[TT_GROUPED_BUF_LEN];

    printf("\n--- set_bool call statistics ---\n");
    printf("%s calls made, %s succeeded\n", tt_format_grouped(transmitted, transmitted_buf),
           tt_format_grouped(succeeded, succeeded_buf));
    if (transmitted > 0 && succeeded == transmitted) {
        printf("RESULT: PASS (%s/%s succeeded)\n", tt_format_grouped(succeeded, succeeded_buf),
               tt_format_grouped(transmitted, transmitted_buf));
    } else {
        printf("RESULT: FAIL (%s/%s succeeded)\n", tt_format_grouped(succeeded, succeeded_buf),
               tt_format_grouped(transmitted, transmitted_buf));
    }
}

static void call(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Client* client = param;

    struct SetBoolRequest request = {.data = next_data};
    tt_ret_t ret = tt_Client_call(client, (struct tt_Request*)&request);
    if (ret == tt_RET_OK) {
        call_data = next_data;
        transmitted++;
        next_data = !next_data;
    } else if (ret == tt_RET_ILLEGAL_STATUS) {
        printf("Previous call still awaiting a response, skipping this interval\n");
    } else {
        printf("Cannot call: %d\n", ret);
    }

    if (target_count == 0 || transmitted < target_count) {
        tt_Node_schedule(node, time + call_interval_ns, call, client);
    }
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-I node_id] [-c count]\n"
            "          [-i interval_seconds] [-n endpoint_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -I  explicit node ID 1-254 (default: auto-detect from -a/-b's subnet)\n");
    fprintf(stderr, "  -c  stop after this many calls (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -i  seconds between calls (default 1)\n");
    fprintf(stderr, "  -n  service name to rendezvous with the server on (default set_bool_server)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->node_id = 0;
    opts->interval_s = 1.0;
    opts->name = "set_bool_server";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;
    opts->count = 0;

    return tt_example_parse_args(argc, argv, opts, TT_EXAMPLE_OPT_COUNT | TT_EXAMPLE_OPT_INTERVAL);
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    target_count = opts.count;

    call_interval_ns = (uint64_t)(opts.interval_s * (double)tt_SECOND);

    _tt_CONFIG.broadcast = opts.broadcast;
    if (opts.port != 0) {
        _tt_CONFIG.port = opts.port;
    }
    if (opts.bind_addr != NULL) {
        _tt_CONFIG.addr = opts.bind_addr;
    }
    if (opts.node_id != 0) {
        _tt_CONFIG.node_id = opts.node_id;
    }
    if (opts.log_level_set) {
        tt_log_set_level(opts.log_level);
    }

    // sigaction (not signal()) so SA_RESTART is off: an interrupted blocking recv
    // returns immediately instead of silently restarting with the same wait.
    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    printf("Node created(#%d)\n", node.id);

    struct tt_Client client;

    ret = tt_Node_create_client(&node, &client, &SetBoolService, opts.name, (tt_CLIENT_CALLBACK)set_bool_callback);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    tt_Node_schedule(&node, tt_get_ns(), call, &client);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    print_result();

    tt_Node_destroy(&node);
    printf("Node destroyed(#%d): %d\n", node.id, ret);

    return 0;
}
