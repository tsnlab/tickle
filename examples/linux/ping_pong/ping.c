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

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static bool g_exit_now = false; // the main loop's actual exit condition - see begin_stopping()

static uint32_t seq = 0;
static uint32_t pending_seq = 0;
static bool pending_counted = true; // was the currently in-flight ping outside warm-up/cool-down?
static uint32_t completed = 0;      // pings whose outcome (reply or drop) is known
static uint64_t send_interval_ns = 0;

static uint32_t target_count = 0;          // live stop-sending threshold (see begin_stopping());
                                           // starts equal to original_target_count
static uint32_t original_target_count = 0; // the actual -c value, immutable after parse_args()
static uint32_t warmup_count = 0;          // -w: initial pings excluded from the statistics
static uint32_t cooldown_count = 0;        // -W: trailing pings excluded from the statistics -
                                           // sent/received *after* the normal stop trigger fires
                                           // (target reached, or Ctrl+C), not reserved out of the
                                           // original -c count - see this file's own comment on
                                           // why (also covers an unbounded -c 0 run stopped with
                                           // Ctrl+C, which reserving from a known count can't).
static bool stopping = false;              // a stop trigger has fired; draining cooldown_count more
static uint64_t cooldown_start = 0;        // sent-count boundary once stopping - >= this is cooldown

static uint64_t sent_total = 0;  // every successful send, including warm-up/cool-down ones -
                                 // drives target_count comparisons (ping()'s/note_completed()'s)
static uint64_t transmitted = 0; // sends within the counted window - what print_statistics() uses
static uint64_t received = 0;    // responses within the counted window
static double rtt_min_ms = -1.0;
static double rtt_max_ms = 0.0;
static double rtt_sum_ms = 0.0;
static double rtt_sum_sq_ms = 0.0;

// Called once, the first time a stop condition is detected (target_count reached, or Ctrl+C) -
// extends target_count by cooldown_count more pings rather than stopping immediately, so those
// trailing samples can actually be observed (and excluded) instead of guessed at in advance. That
// also means this covers an unbounded (-c 0) run stopped with Ctrl+C the same way it covers a
// fixed -c count - either way, "cooldown_count more from wherever we are now" needs no advance
// knowledge of when the run will end. cooldown_count == 0 still goes through here rather than
// exiting directly - target_count just stays where it is, so note_completed() still waits for the
// already-in-flight ping's own outcome before actually exiting, instead of cutting it off
// mid-flight the way the old direct-g_interrupted-stops-the-loop code used to. Idempotent - only
// the first trigger takes effect.
static void begin_stopping(void) {
    if (stopping) {
        return;
    }
    stopping = true;
    cooldown_start = sent_total;
    target_count = (uint32_t)sent_total + cooldown_count;
}

// The exit decision belongs here (once every outcome up to target_count is known), not wherever
// begin_stopping() gets triggered from - see ping()'s own call site for why the *trigger* has to
// happen at send time, synchronously before that same call decides whether to schedule another.
static void note_completed(void) {
    completed++;
    if (stopping && completed >= target_count) {
        g_exit_now = true;
    }
}

static void ping_callback(struct tt_Client* client, int8_t return_code, struct PingPongResponse* response) {
    (void)client;

    const char* tag = "";
    if (!pending_counted) {
        tag = sent_total <= warmup_count ? " (warmup)" : " (cooldown)";
    }

    if (return_code == 0 && response == NULL) {
        printf("Request timeout for icmp_seq=%u (dropped)%s\n", pending_seq, tag);
        note_completed();
        return;
    }
    if (return_code != 0) {
        printf("Error, return_code: %d (icmp_seq=%u dropped)%s\n", return_code, pending_seq, tag);
        note_completed();
        return;
    }

    double rtt_ms = (double)(tt_get_ns() - response->timestamp) / (double)tt_MILLISECOND;

    printf("seq=%u time=%.3f ms%s\n", response->seq, rtt_ms, tag);

    if (pending_counted) {
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

    note_completed();
}

static void ping(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Client* client = param;

    uint32_t this_seq = seq;
    struct PingPongRequest request = {.seq = this_seq, .timestamp = tt_get_ns()};
    tt_ret_t ret = tt_Client_call(client, (struct tt_Request*)&request);
    if (ret == tt_RET_OK) {
        seq++;
        bool in_warmup = sent_total < warmup_count;
        bool in_cooldown = stopping && sent_total >= cooldown_start;
        pending_counted = !in_warmup && !in_cooldown;
        if (pending_counted) {
            transmitted++;
        }
        sent_total++;
        pending_seq = this_seq;

        // Checked here (send time), not in note_completed() (receive time): target_count needs
        // to already reflect the extension by the time this same call's own "should I send
        // another" check below runs, or that check would stop scheduling before cooldown_count's
        // extra pings ever go out - see begin_stopping()'s own comment on the rest of the design.
        if (!stopping && original_target_count > 0 && sent_total >= original_target_count) {
            begin_stopping();
        }
    } else if (ret == tt_RET_ILLEGAL_STATUS) {
        printf("Previous ping still awaiting a reply, skipping this interval\n");
    } else {
        printf("Cannot send ping: %d\n", ret);
    }

    if (target_count == 0 || sent_total < target_count) {
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
    double elapsed_ms = (double)(tt_get_ns() - start_time) / (double)tt_MILLISECOND;

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
            "          [-i interval_seconds] [-w warmup_count] [-W cooldown_count]\n"
            "          [-n endpoint_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -I  explicit node ID 1-254 (default: auto-detect from -a/-b's subnet)\n");
    fprintf(stderr, "  -c  stop after this many pings (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -i  seconds between pings (default 1)\n");
    fprintf(stderr, "  -w  exclude this many initial pings from the statistics (default 0)\n");
    fprintf(stderr, "  -W  keep pinging (uncounted) this many more times after the stop\n");
    fprintf(stderr, "      trigger (-c reached, or Ctrl+C) before actually exiting (default 0)\n");
    fprintf(stderr, "  -n  service name to rendezvous with pong on (default ping_pong_server)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->node_id = 0;
    opts->interval_s = 1.0;
    opts->warmup = 0.0;
    opts->cooldown = 0.0;
    opts->name = "ping_pong_server";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;
    opts->count = 0;

    return tt_example_parse_args(argc, argv, opts,
                                 TT_EXAMPLE_OPT_COUNT | TT_EXAMPLE_OPT_INTERVAL | TT_EXAMPLE_OPT_WARMUP_COOLDOWN);
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    original_target_count = opts.count;
    target_count = opts.count;
    warmup_count = (uint32_t)opts.warmup;
    cooldown_count = (uint32_t)opts.cooldown;

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
    tt_Node_schedule(&node, start_time, ping, &client);

    ret = tt_RET_OK;
    while (!g_exit_now && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
        // Checked here (right after poll() returns), not inside ping()'s own schedule: this runs
        // every iteration regardless of how long until the next scheduled ping, so Ctrl+C is
        // caught right away - see begin_stopping()'s own comment on why immediacy matters (it
        // needs sent_total to reflect "now", not whatever it was as of the last scheduled tick).
        if (g_interrupted && !stopping) {
            begin_stopping();
        }
    }

    print_statistics(start_time);

    tt_Node_destroy(&node);

    return 0;
}
