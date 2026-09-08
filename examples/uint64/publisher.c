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
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>

#include "../common/cli_opts.h"
#include "UInt64.h"

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint32_t target_count = 0; // 0 = unlimited
static uint32_t transmitted = 0;
static uint64_t publish_interval_ns = 0;

static void publish(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;

    const uint64_t example_data = 0xdeadbeef;
    struct UInt64Data data = {.data = example_data + transmitted};
    tt_ret_t ret = tt_Publisher_publish(pub, (struct tt_Data*)&data);
    if (ret == tt_RET_OK) {
        transmitted++;
    } else {
        printf("Cannot publish: %d\n", ret);
    }

    if (target_count == 0 || transmitted < target_count) {
        tt_Node_schedule(node, time + publish_interval_ns, publish, pub);
    } else {
        g_interrupted = 1;
    }
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-c count] [-i interval_seconds]\n"
            "          [-n topic_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -c  stop after publishing this many messages (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -i  seconds between publishes (default 1)\n");
    fprintf(stderr, "  -n  topic name to publish on (default uint64_topic)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->interval_s = 1.0;
    opts->name = "uint64_topic";
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

    publish_interval_ns = (uint64_t)(opts.interval_s * (double)tt_SECOND);

    _tt_CONFIG.broadcast = opts.broadcast;
    if (opts.port != 0) {
        _tt_CONFIG.port = opts.port;
    }
    if (opts.bind_addr != NULL) {
        _tt_CONFIG.addr = opts.bind_addr;
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

    struct tt_Publisher pub;

    ret = tt_Node_create_publisher(&node, &pub, &UInt64Topic, opts.name);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    tt_Node_schedule(&node, tt_get_ns(), publish, &pub);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    tt_Node_destroy(&node);
    printf("Node destroyed(#%d): %d\n", node.id, ret);

    return 0;
}
