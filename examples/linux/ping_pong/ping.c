/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <math.h>
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

#include "../../format.h"
#include "../common/cli_opts.h"
#include "PingPong.h"

static volatile sig_atomic_t g_interrupted = 0; // raw SIGINT flag - see main()'s own comment
static bool g_exit_now = false;                 // the main loop's actual exit condition

static bool stopping = false;       // a stop trigger has fired; draining cooldown_s more now
static uint64_t cooldown_start = 0; // ns timestamp once stopping - >= this is cooldown

static uint64_t g_start_time = 0; // set once in main() - anchors warm-up (ping sends immediately
                                  // at startup, unlike perf_server, so there's no "wait for the
                                  // peer" gap to worry about the way first_recv_time exists for)
static double warmup_s = 0.0;     // -w: seconds after start before counting starts
static double cooldown_s = 0.0;   // -W: seconds to keep pinging (uncounted) after the stop
                                  // trigger (-c/-d reached, or Ctrl+C) before actually exiting -
                                  // matches perf_server.c's own -w/-W exactly (see its comment).
static uint32_t target_count = 0; // -c, 0 = unlimited - independent of warm-up/cool-down, an
                                  // optional extra cap on total pings sent regardless of time

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void finish_cooldown(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_exit_now = true;
}

// Called once, the first time a stop condition is detected (-c/-d reached, or Ctrl+C) - rather
// than exiting immediately, keeps pinging for cooldown_s more seconds (uncounted - see ping()'s
// own gating) before actually exiting. Works the same way whether the run was bounded (-c/-d) or
// not (stopped with Ctrl+C), since either way this only ever needs "cooldown_s seconds from now",
// never advance knowledge of when the run will end. Idempotent - only the first trigger takes
// effect. Exactly mirrors perf_server.c's own begin_stopping().
static void begin_stopping(struct tt_Node* node, uint64_t time) {
    if (stopping) {
        return;
    }
    stopping = true;
    cooldown_start = time;
    if (cooldown_s <= 0.0) {
        g_exit_now = true;
        return;
    }
    tt_Node_schedule(node, time + (uint64_t)(cooldown_s * (double)tt_SECOND), finish_cooldown, NULL);
}

static void handle_duration_elapsed(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    begin_stopping(node, time);
}

static uint32_t seq = 0; // also this run's total successful-send count (0-based, pre-increment)
static uint32_t pending_seq = 0;
static bool pending_in_warmup = false; // classified at send time - see ping()'s own comment
static bool pending_in_cooldown = false;
static uint64_t send_interval_ns = 0;

static uint64_t transmitted = 0; // sends within the counted window - what print_statistics() uses
static uint64_t received = 0;    // responses within the counted window
static double rtt_min_ms = -1.0;
static double rtt_max_ms = 0.0;
static double rtt_sum_ms = 0.0;
static double rtt_sum_sq_ms = 0.0;

static void ping_callback(struct tt_Client* client, int8_t return_code, struct PingPongResponse* response) {
    (void)client;

    const char* tag = "";
    if (pending_in_warmup) {
        tag = " (warmup)";
    } else if (pending_in_cooldown) {
        tag = " (cooldown)";
    }

    if (return_code == 0 && response == NULL) {
        printf("Request timeout for icmp_seq=%u (dropped)%s\n", pending_seq, tag);
        return;
    }
    if (return_code != 0) {
        printf("Error, return_code: %d (icmp_seq=%u dropped)%s\n", return_code, pending_seq, tag);
        return;
    }

    double rtt_ms = (double)(tt_get_ns() - response->timestamp) / (double)tt_MILLISECOND;

    printf("seq=%u time=%.3f ms%s\n", response->seq, rtt_ms, tag);

    if (!pending_in_warmup && !pending_in_cooldown) {
        received++;
        if (rtt_min_ms < 0.0 || rtt_ms < rtt_min_ms) {
            rtt_min_ms = rtt_ms;
        }
        if (rtt_ms > rtt_max_ms) {
            rtt_max_ms = rtt_ms;
        }
        rtt_sum_ms += rtt_ms;
        rtt_sum_sq_ms += rtt_ms * rtt_ms;
    }
}

static void ping(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Client* client = param;

    uint32_t this_seq = seq;
    struct PingPongRequest request = {.seq = this_seq, .timestamp = tt_get_ns()};
    tt_ret_t ret = tt_Client_call(client, (struct tt_Request*)&request);
    if (ret == tt_RET_OK) {
        seq++;
        double elapsed_s = (double)(time - g_start_time) / (double)tt_SECOND;
        pending_in_warmup = elapsed_s < warmup_s;
        pending_in_cooldown = stopping && time >= cooldown_start;
        if (!pending_in_warmup && !pending_in_cooldown) {
            transmitted++;
        }
        pending_seq = this_seq;

        // Checked here (send time), not from note_completed()-style receive-time bookkeeping:
        // ping()'s own "should I send another" check just below needs `stopping` to already be
        // set if this send just crossed target_count, or it would stop scheduling before
        // cooldown_s's extra pings ever go out.
        if (!stopping && target_count > 0 && seq >= target_count) {
            begin_stopping(node, time);
        }
    } else if (ret == tt_RET_ILLEGAL_STATUS) {
        printf("Previous ping still awaiting a reply, skipping this interval\n");
    } else {
        printf("Cannot send ping: %d\n", ret);
    }

    // Once stopping, target_count (an original send-count cap, if any) no longer gates further
    // sends - cooldown_s's own timer (finish_cooldown(), scheduled from begin_stopping()) is what
    // eventually sets g_exit_now and ends this loop instead.
    if (!g_exit_now && (stopping || target_count == 0 || seq < target_count)) {
        tt_Node_schedule(node, time + send_interval_ns, ping, client);
    }
}

// The verifying side of the ping/pong round trip: pong.c never logs per-request (see its own
// comment), so this ping's own tally - not pong.log - is the real evidence a round trip
// happened. Latency is what this pair measures, so RESULT reports the numbers themselves rather
// than a pass/fail verdict (see set_bool/uint64's client.c/subscriber.c for the functional-
// verification counterpart) - a reader (human or CI log scraper) judges "good" or "bad" against
// the actual rtt/loss values.
static void print_statistics(uint64_t start_time) {
    char transmitted_buf[TT_GROUPED_BUF_LEN];
    char received_buf[TT_GROUPED_BUF_LEN];
    uint64_t lost = transmitted - received;
    double loss_pct = transmitted > 0 ? (100.0 * (double)lost / (double)transmitted) : 0.0;

    // The counted window excludes warm-up/cool-down, so its duration is measured the same way -
    // from when warm-up ended to when cool-down began (or now, if this somehow got here without
    // ever stopping) - not the raw start_time-to-now process span. Mirrors perf_server.c's own
    // print_summary() exactly.
    uint64_t window_start = start_time + (uint64_t)(warmup_s * (double)tt_SECOND);
    uint64_t window_end = stopping ? cooldown_start : tt_get_ns();
    double elapsed_ms = window_end > window_start ? (double)(window_end - window_start) / (double)tt_MILLISECOND : 0.0;

    printf("\n--- ping statistics ---\n");
    printf("%s packets transmitted, %s received, %.0f%% packet loss, time %.0fms\n",
           tt_format_grouped(transmitted, transmitted_buf), tt_format_grouped(received, received_buf), loss_pct,
           elapsed_ms);

    if (received > 0) {
        double avg = rtt_sum_ms / (double)received;
        double variance = (rtt_sum_sq_ms / (double)received) - (avg * avg);
        double mdev = variance > 0.0 ? sqrt(variance) : 0.0;
        printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n", rtt_min_ms, avg, rtt_max_ms, mdev);
        printf("RESULT: sent=%s recv=%s loss_pct=%.0f rtt_avg_ms=%.3f rtt_max_ms=%.3f\n",
               tt_format_grouped(transmitted, transmitted_buf), tt_format_grouped(received, received_buf), loss_pct,
               avg, rtt_max_ms);
    } else {
        printf("RESULT: sent=%s recv=%s loss_pct=%.0f\n", tt_format_grouped(transmitted, transmitted_buf),
               tt_format_grouped(received, received_buf), loss_pct);
    }
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-I node_id] [-c count]\n"
            "          [-i interval_seconds] [-d duration_seconds] [-w warmup_seconds]\n"
            "          [-W cooldown_seconds] [-n endpoint_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -I  explicit node ID 1-254 (default: auto-detect from -a/-b's subnet)\n");
    fprintf(stderr, "  -c  stop after this many pings (default 0 = unlimited - use -d/Ctrl+C)\n");
    fprintf(stderr, "  -i  seconds between pings (default 1)\n");
    fprintf(stderr, "  -d  stop after this many seconds (default 0 = run until -c/Ctrl+C)\n");
    fprintf(stderr, "  -w  exclude this many initial seconds from the statistics (default 0)\n");
    fprintf(stderr, "  -W  keep pinging (uncounted) this many more seconds after the stop\n");
    fprintf(stderr, "      trigger (-c/-d reached, or Ctrl+C) before actually exiting (default 0)\n");
    fprintf(stderr, "  -n  service name to rendezvous with pong on (default ping_pong_server)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->node_id = 0;
    opts->interval_s = 1.0;
    opts->duration_s = 0.0;
    opts->warmup = 0.0;
    opts->cooldown = 0.0;
    opts->name = "ping_pong_server";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;
    opts->count = 0;

    return tt_example_parse_args(argc, argv, opts,
                                 TT_EXAMPLE_OPT_COUNT | TT_EXAMPLE_OPT_INTERVAL | TT_EXAMPLE_OPT_DURATION |
                                     TT_EXAMPLE_OPT_WARMUP_COOLDOWN);
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    target_count = opts.count;
    warmup_s = opts.warmup;
    cooldown_s = opts.cooldown;

    send_interval_ns = (uint64_t)(opts.interval_s * (double)tt_SECOND);

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
    ret = tt_Node_create_client(&node, &client, &PingPongService, opts.name, (tt_CLIENT_CALLBACK)ping_callback);
    if (ret != 0) {
        printf("Cannot create client: %d\n", ret);
        return ret;
    }

    uint64_t start_time = tt_get_ns();
    g_start_time = start_time;
    tt_Node_schedule(&node, start_time, ping, &client);
    if (opts.duration_s > 0.0) {
        // -d measures the real (post-warm-up) data window, not time since start_time - a plain
        // start_time + duration_s trigger would let warmup_s eat into it, e.g. -d 60 -w 5 actually
        // only measuring 55s. + warmup_s here instead keeps -d's own meaning exactly "seconds of
        // counted data" regardless of what -w is.
        uint64_t stop_at = start_time + (uint64_t)((warmup_s + opts.duration_s) * (double)tt_SECOND);
        tt_Node_schedule(&node, stop_at, handle_duration_elapsed, NULL);
    }

    ret = tt_RET_OK;
    while (!g_exit_now && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
        // Checked here (right after poll() returns), not inside ping()'s own schedule: this runs
        // every iteration regardless of how long until the next scheduled ping, so Ctrl+C is
        // caught right away instead of up to one send_interval_ns late.
        if (g_interrupted && !stopping) {
            begin_stopping(&node, tt_get_ns());
        }
    }

    print_statistics(start_time);

    tt_Node_destroy(&node);

    return 0;
}
