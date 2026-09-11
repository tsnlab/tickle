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
#include "PingPong.h"

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void handle_duration_elapsed(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

static uint32_t handled = 0;

static int8_t pong_callback(struct tt_Server* server, struct PingPongRequest* request,
                            struct PingPongResponse* response) {
    (void)server;
    response->seq = request->seq;
    response->timestamp = request->timestamp;
    handled++;
    return 0;
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-I node_id]\n"
            "          [-d duration_seconds] [-n endpoint_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -I  explicit node ID 1-254 (default: auto-detect from -a/-b's subnet)\n");
    fprintf(stderr, "  -d  exit automatically after this many seconds (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -n  service name to rendezvous with ping on (default ping_pong_server)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->node_id = 0;
    opts->duration_s = 0.0;
    opts->name = "ping_pong_server";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;

    return tt_example_parse_args(argc, argv, opts, TT_EXAMPLE_OPT_DURATION);
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }

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

    struct tt_Server server;
    ret = tt_Node_create_server(&node, &server, &PingPongService, opts.name, (tt_SERVER_CALLBACK)pong_callback);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    if (opts.duration_s > 0.0) {
        tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(opts.duration_s * (double)tt_SECOND), handle_duration_elapsed,
                         NULL);
    }

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    // Informational only - this side has no pass/fail verdict of its own; ping's own RESULT line
    // (see ping.c's print_statistics()) is what actually measures the round trip.
    char handled_buf[TT_GROUPED_BUF_LEN];
    printf("\nping_pong server: answered %s request(s)\n", tt_format_grouped(handled, handled_buf));

    tt_Node_destroy(&node);
    printf("Node destroyed(#%d): %d\n", node.id, ret);

    return 0;
}
