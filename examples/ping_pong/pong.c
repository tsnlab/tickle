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

static int8_t pong_callback(struct tt_Server* server, struct PingPongRequest* request,
                            struct PingPongResponse* response) {
    (void)server;
    response->seq = request->seq;
    response->timestamp = request->timestamp;
    return 0;
}

static bool parse_log_level(const char* str, tt_LogLevel* level) {
    if (strcmp(str, "debug") == 0) {
        *level = TT_LOG_DEBUG;
    } else if (strcmp(str, "info") == 0) {
        *level = TT_LOG_INFO;
    } else if (strcmp(str, "warning") == 0) {
        *level = TT_LOG_WARNING;
    } else if (strcmp(str, "error") == 0) {
        *level = TT_LOG_ERROR;
    } else if (strcmp(str, "none") == 0) {
        *level = TT_LOG_NONE;
    } else {
        return false;
    }
    return true;
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-d duration_seconds]\n"
            "          [-n endpoint_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -d  exit automatically after this many seconds (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -n  service name to rendezvous with ping on (default ping_pong_server)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

struct cli_options {
    char* broadcast;
    int port;          // 0 = keep the compiled-in default
    char* bind_addr;   // NULL = keep the compiled-in default
    double duration_s; // 0 = run until Ctrl+C
    char* endpoint_name;
    tt_LogLevel log_level;
    bool log_level_set;
};

// Returns 0 on success, non-zero if argv held an unrecognized/incomplete option.
static int parse_args(int argc, char** argv, struct cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->duration_s = 0.0;
    opts->endpoint_name = "ping_pong_server";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            opts->broadcast = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            opts->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            opts->bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            opts->duration_s = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            opts->endpoint_name = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            if (!parse_log_level(argv[++i], &opts->log_level)) {
                return 1;
            }
            opts->log_level_set = true;
        } else {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    struct cli_options opts;
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
    ret =
        tt_Node_create_server(&node, &server, &PingPongService, opts.endpoint_name, (tt_SERVER_CALLBACK)pong_callback);
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

    tt_Node_destroy(&node);
    printf("Node destroyed(#%d): %d\n", node.id, ret);

    return 0;
}
