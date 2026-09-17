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
#include "Bulk.h"

static volatile sig_atomic_t g_interrupted = 0; // raw SIGINT flag - see main()'s own comment
static bool g_exit_now = false;                 // the main loop's actual exit condition

static bool stopping = false;       // a stop trigger has fired; draining cooldown_s more now
static uint64_t cooldown_start = 0; // ns timestamp once stopping - >= this is cooldown

static double warmup_s = 0.0;   // -w: seconds after the first real message before counting starts
static double cooldown_s = 0.0; // -W: seconds to keep receiving (uncounted) after the stop
                                // trigger (-d elapsed, or Ctrl+C) before actually exiting - see
                                // this file's own comment on why this isn't computed by looking
                                // backward from a known -d instead.
static double duration_s = 0.0; // -d: seconds of real (post-warm-up) data before the stop
                                // trigger - see bulk_callback()'s own comment on why this is
                                // scheduled dynamically off the first real message, not up front.

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

// Called once, the first time a stop condition is detected (-d elapsed, or Ctrl+C) - rather than
// exiting immediately, keeps receiving for cooldown_s more seconds (uncounted - see
// bulk_callback()'s own gating) before actually exiting. Works the same way whether the run was
// bounded (-d) or not (stopped with Ctrl+C), since either way this only ever needs "cooldown_s
// seconds from now", never advance knowledge of when the run will end. Idempotent - only the
// first trigger takes effect.
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

static bool have_first = false;
static uint32_t expected_seq = 0;
// A forward gap used to be counted as dropped the instant it was skipped over, which never gave
// a RELIABLE Publisher/Subscriber's own retransmission (QoS roadmap #5) any chance to actually
// land before being written off - every recovered sample still counted once, permanently, making
// RELIABLE's own reported loss_pct look barely better (once its earlier ~100%-from-underflow bug
// was fixed) than BEST_EFFORT's, when the real recovery rate on this rig's own real hardware
// (RTT far under tt_RELIABLE_DEADLINE's own auto retry interval, tt_MAX_RELIABLE_HISTORY=8 comfortably
// covering it at the 20ms/msg pacing run_perf.sh's loss scenarios use) turned out to be far higher.
// pending_bitmap below defers that judgment instead - bit j set: (expected_seq + 1 + j) has
// already been received, out of order ahead of the watermark. The same problem, and the same
// underlying fix (an update advancing the watermark must also realign the bitmap - see
// track_arrival()'s own comment), as tt_Subscriber's own ack_seq_no/received_bitmap tracking
// (tickle.c) - but a deliberately different bit-to-sequence-number offset (this file's own "+1+j"
// vs. tickle.c's "+j"): tickle.c's has to match tt_AckNackHeader's own wire format exactly for a
// trivial `~received_bitmap` inversion to stay correct; this one is purely an in-process counter
// with no wire encoding step, so it keeps its own original, simpler-to-derive convention instead.
#define GAP_WINDOW_BITS 64
static uint64_t pending_bitmap = 0;
static uint64_t first_recv_time = 0; // ns timestamp of the first real message - anchors warm-up,
                                     // not this process's own start_time (see this file's own
                                     // comment: perf_server's -d intentionally runs longer than
                                     // perf_client's, so "since I started" would count idle time
                                     // before perf_client even begins sending as warm-up).

// A safety-net timeout, scheduled up front (main()) independent of whether any real traffic ever
// shows up - covers the degenerate "no message ever arrived" case, which bulk_callback()'s own
// (more precise, warm-up-aware) scheduling of handle_duration_elapsed can never reach on its own,
// since that one only ever fires off a message that already arrived. Only actually stops if
// have_first is still false when it fires - otherwise a no-op, since real traffic already
// scheduled the accurate trigger by then. Deliberately well past duration_s (see main()'s own
// scheduling of this) so it can never preempt the accurate one once real traffic does show up.
static void handle_no_traffic_timeout(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;
    if (!have_first) {
        begin_stopping(node, time);
    }
}

static uint64_t total_received_msgs = 0;
static uint64_t total_received_bytes = 0;
static uint64_t total_dropped = 0;

static uint64_t interval_received_msgs = 0;
static uint64_t interval_received_bytes = 0;

// One-way delivery latency: `time` (bulk_callback's own parameter) is the *sender's* clock at
// publish (tt_Publisher_publish()'s data_header->timestamp = tt_get_ns() - tickle.c), so this is
// only meaningful when both Pis' clocks are reasonably synchronized (NTP) - see README.md's own
// caveat on this. Even without that, it's still a fair *relative* comparison across loss levels
// and BEST_EFFORT vs RELIABLE on this same pair of machines, which is what run_perf.sh's own
// loss-injection scenarios actually use it for - a fixed clock-offset error cancels out when
// comparing two runs against each other, only the absolute number is suspect.
static uint64_t latency_sum_ns = 0;
static uint64_t latency_count = 0;
static uint64_t latency_min_ns = UINT64_MAX;
static uint64_t latency_max_ns = 0;

// Tracks one arrival's effect on expected_seq/pending_bitmap, deferring a forward gap's "is this
// really lost" judgment for up to GAP_WINDOW_BITS more messages instead of counting it the instant
// it's skipped over. Returns how many messages should now count as permanently, newly dropped - 0
// most of the time; a gap only resolves to a real drop once it falls out the far end of the
// window, either right here (the overflow branch) or in finalize_gap_tracking() at the very end
// of the run.
static uint32_t track_arrival(uint32_t seq) {
    if (!have_first) {
        expected_seq = seq + 1;
        return 0;
    }

    int32_t delta = (int32_t)(seq - expected_seq);
    if (delta == 0) {
        expected_seq++;
        // Realign: bit j must always mean "received(expected_seq + j)", including j=0 - so this
        // needs its own unconditional shift right here, not only inside the while loop below.
        // Missing this shift is exactly the bug tt_Subscriber's own update_reliable_ack()
        // (tickle.c) had until this same run_perf.sh loss-injection scenario surfaced it: every
        // bit still tracking a *different*, still-outstanding gap silently drifted by one
        // position the moment any earlier gap got filled this way, without ever crashing or
        // erroring - just quietly asking for (or reporting on) the wrong sequence number.
        pending_bitmap >>= 1;
        while (pending_bitmap & 1) { // absorb whatever out-of-order run already follows it
            pending_bitmap >>= 1;
            expected_seq++;
        }
        return 0;
    }
    if (delta < 0) {
        // A late arrival behind the current watermark - almost always a RELIABLE retransmission
        // that successfully recovered an earlier gap. Clear its pending bit (it turned out not to
        // be lost after all) rather than rewinding expected_seq or counting it again - the same
        // underflow-avoidance reasoning this function's own predecessor already needed, just
        // folded into the windowed bitmap instead of a one-shot decision.
        uint32_t behind = (uint32_t)(-delta);
        if (behind <= GAP_WINDOW_BITS) {
            pending_bitmap &= ~(1ULL << (behind - 1));
        }
        return 0;
    }
    if ((uint32_t)delta <= GAP_WINDOW_BITS) {
        pending_bitmap |= (1ULL << ((uint32_t)delta - 1)); // deferred - might still be retransmitted
        return 0;
    }

    // Gap wider than the tracking window - give up on it immediately rather than sliding the
    // window bit by bit. Realistically never hit at this rig's own loss rates/message sizes:
    // GAP_WINDOW_BITS is already far more slack than RELIABLE's own tt_MAX_RELIABLE_HISTORY (8)
    // or retry budget could ever fill. `delta` values are missing - expected_seq itself plus the
    // delta-1 that follow it up to (but not including) seq, which just arrived - and
    // expected_seq itself is never covered by any bit (bit 0 means expected_seq+1, not
    // expected_seq), so no adjustment for it is needed here the way finalize_gap_tracking() below
    // needs one.
    uint32_t dropped_now = (uint32_t)delta;
    for (int i = 0; i < GAP_WINDOW_BITS; i++) {
        if (pending_bitmap & (1ULL << i)) {
            dropped_now--; // already confirmed received - don't also call it dropped
        }
    }
    pending_bitmap = 0;
    // TEMPORARY diagnostic (QoS roadmap #5 loss-injection investigation) - printf, not a
    // tickle/log.h call: perf_server.c only has the public API (tt_log_set_level()), not the
    // internal TT_LOG_WARNING() macro tickle.c itself uses.
    printf("DIAG track_arrival overflow: expected_seq=%u seq=%u dropped_now=%u\n", expected_seq, seq, dropped_now);
    expected_seq = seq + 1;
    return dropped_now;
}

// Called once, from print_summary() - anything still waiting in the window when the run ends (no
// more data coming to either confirm or resolve it) is now given up on for good.
static uint32_t finalize_gap_tracking(void) {
    if (pending_bitmap == 0) {
        return 0;
    }
    // TEMPORARY diagnostic (QoS roadmap #5 loss-injection investigation) - see track_arrival()'s
    // own overflow-branch diagnostic for why printf, not a tickle/log.h call.
    printf("DIAG finalize_gap_tracking: expected_seq=%u pending_bitmap=%016llx\n", expected_seq,
           (unsigned long long)pending_bitmap);
    // expected_seq itself is a confirmed-missing slot whenever anything is pending ahead of it
    // (that's exactly what a nonzero pending_bitmap here means) - it's never covered by a bit of
    // its own (bit 0 means expected_seq+1), so it needs its own explicit +1.
    uint32_t dropped_now = 1;
    int highest = GAP_WINDOW_BITS - 1;
    while (highest >= 0 && !((pending_bitmap >> highest) & 1)) {
        highest--;
    }
    for (int i = 0; i <= highest; i++) {
        if (!((pending_bitmap >> i) & 1)) {
            dropped_now++;
        }
    }
    pending_bitmap = 0;
    return dropped_now;
}

static void bulk_callback(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct BulkData* data) {
    (void)seq_no; // truncated to 16 bits by the framework; data->seq is the real 32-bit one

    if (!have_first) {
        first_recv_time = time;
        if (duration_s > 0.0) {
            // Scheduled here (once, off the first real message), not up front in main(): -d
            // measures the real data window, not time since this process's own start_time - a
            // start_time + duration_s trigger would let warmup_s (itself anchored to
            // first_recv_time, not start_time - see that variable's own comment) eat into it, on
            // top of the pre-existing "-d intentionally runs longer than perf_client's" gap.
            // + warmup_s here instead keeps -d's own meaning exactly "seconds of counted data".
            uint64_t stop_at = first_recv_time + (uint64_t)((warmup_s + duration_s) * (double)tt_SECOND);
            tt_Node_schedule(sub->node, stop_at, handle_duration_elapsed, NULL);
        }
    }

    uint32_t gap_count = track_arrival(data->seq);
    have_first = true;

    interval_received_msgs++;
    interval_received_bytes += data->payload_count;

    bool in_warmup = (double)(time - first_recv_time) / (double)tt_SECOND < warmup_s;
    bool in_cooldown = stopping && time >= cooldown_start;
    if (!in_warmup && !in_cooldown) {
        total_received_msgs++;
        total_received_bytes += data->payload_count;
        total_dropped += gap_count;

        uint64_t now = tt_get_ns();
        if (now > time) { // guards a clock skew that would otherwise underflow this subtraction
            uint64_t latency_ns = now - time;
            latency_sum_ns += latency_ns;
            latency_count++;
            if (latency_ns < latency_min_ns) {
                latency_min_ns = latency_ns;
            }
            if (latency_ns > latency_max_ns) {
                latency_max_ns = latency_ns;
            }
        }
    }
}

static void report(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;

    char recv_buf[TT_GROUPED_BUF_LEN];
    char dropped_buf[TT_GROUPED_BUF_LEN];
    char megabytes_buf[TT_GROUPED_F3_BUF_LEN];
    char mbps_buf[TT_GROUPED_F3_BUF_LEN];
    const double bytes_per_mb = 1e6;
    double megabytes = (double)interval_received_bytes / bytes_per_mb;
    double mbps = ((double)interval_received_bytes * 8) / bytes_per_mb;

    bool in_warmup = have_first && (double)(time - first_recv_time) / (double)tt_SECOND < warmup_s;
    bool in_cooldown = stopping && time >= cooldown_start;
    const char* tag = "";
    if (in_warmup) {
        tag = " (warmup)";
    } else if (in_cooldown) {
        tag = " (cooldown)";
    }

    printf("recv %s msgs, %s MB, %s Mbps this interval (%s dropped so far)%s\n",
           tt_format_grouped(interval_received_msgs, recv_buf), tt_format_grouped_f3(megabytes, megabytes_buf),
           tt_format_grouped_f3(mbps, mbps_buf), tt_format_grouped(total_dropped, dropped_buf), tag);

    interval_received_msgs = 0;
    interval_received_bytes = 0;

    tt_Node_schedule(node, time + tt_SECOND, report, NULL);
}

// The verifying side of the perf round trip: it can see drops (perf_client.c can't - it never
// hears back), so this is the authoritative measurement, same role ping.c's print_statistics()
// plays for latency. Still numbers, not a pass/fail verdict - throughput/loss is a spectrum a
// human or CI log scraper judges against a threshold, not a binary outcome the way "did the
// response ever arrive at all" is for set_bool/uint64.
static void print_summary(uint64_t start_time) {
    char recv_buf[TT_GROUPED_BUF_LEN];
    char dropped_buf[TT_GROUPED_BUF_LEN];
    char megabytes_buf[TT_GROUPED_F3_BUF_LEN];
    char avg_mbps_buf[TT_GROUPED_F3_BUF_LEN];
    const double bytes_per_mb = 1e6;
    const double percent_scale = 100.0;

    // The counted window excludes warm-up/cool-down, so its duration is measured the same way -
    // from when warm-up ended (first real message + warmup_s) to when cool-down began (or now, if
    // this somehow got here without ever stopping - shouldn't normally happen, but a safe
    // fallback) - not this process's own start_time/-d window, which can run measurably longer
    // than the real traffic (see first_recv_time's own comment on why).
    uint64_t window_start = have_first ? first_recv_time + (uint64_t)(warmup_s * (double)tt_SECOND) : start_time;
    uint64_t window_end = stopping ? cooldown_start : tt_get_ns();
    double elapsed_s = window_end > window_start ? (double)(window_end - window_start) / (double)tt_SECOND : 0.0;

    // Nothing more is coming now - anything track_arrival() was still deferring judgment on (a
    // gap that might yet have been a RELIABLE retransmission still in flight) is given up on.
    total_dropped += finalize_gap_tracking();

    double megabytes = (double)total_received_bytes / bytes_per_mb;
    double avg_mbps = elapsed_s > 0.0 ? ((double)total_received_bytes * 8) / bytes_per_mb / elapsed_s : 0.0;
    uint64_t expected_total = total_received_msgs + total_dropped;
    double loss_pct = expected_total > 0 ? (percent_scale * (double)total_dropped / (double)expected_total) : 0.0;

    const double ns_per_ms = 1e6;
    double avg_latency_ms = latency_count > 0 ? (double)latency_sum_ns / (double)latency_count / ns_per_ms : 0.0;
    double min_latency_ms = latency_count > 0 ? (double)latency_min_ns / ns_per_ms : 0.0;
    double max_latency_ms = latency_count > 0 ? (double)latency_max_ns / ns_per_ms : 0.0;

    printf("\n--- bulk_topic receive statistics ---\n");
    printf("%s messages received, %s dropped, %.1f%% loss, %s MB, %.3f sec, avg %s Mbps\n",
           tt_format_grouped(total_received_msgs, recv_buf), tt_format_grouped(total_dropped, dropped_buf), loss_pct,
           tt_format_grouped_f3(megabytes, megabytes_buf), elapsed_s, tt_format_grouped_f3(avg_mbps, avg_mbps_buf));
    printf("one-way latency (sender clock -> here, NTP-dependent - see this file's own comment): "
           "min/avg/max = %.3f/%.3f/%.3f ms\n",
           min_latency_ms, avg_latency_ms, max_latency_ms);
    printf("RESULT: recv=%s dropped=%s loss_pct=%.1f avg_mbps=%s avg_latency_ms=%.3f\n",
           tt_format_grouped(total_received_msgs, recv_buf), tt_format_grouped(total_dropped, dropped_buf), loss_pct,
           tt_format_grouped_f3(avg_mbps, avg_mbps_buf), avg_latency_ms);
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-I node_id]\n"
            "          [-d duration_seconds] [-w warmup_seconds] [-W cooldown_seconds]\n"
            "          [-n topic_name] [-l log_level] [-R]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -I  explicit node ID 1-254 (default: auto-detect from -a/-b's subnet)\n");
    fprintf(stderr, "  -d  exit automatically after this many seconds (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -w  seconds after the first real message to start counting (default 0)\n");
    fprintf(stderr, "  -W  seconds to keep receiving (uncounted) after the stop trigger before\n");
    fprintf(stderr, "      actually exiting (default 0 = stop immediately)\n");
    fprintf(stderr, "  -n  topic name to subscribe to (default bulk_topic)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
    fprintf(stderr, "  -R  RELIABLE instead of BEST_EFFORT delivery (QoS roadmap #5, rmw_tickle/PLAN.md) -\n"
                    "      must match perf_client's own -R\n");
}

static int parse_args(int argc, char** argv, struct tt_example_cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->node_id = 0;
    opts->duration_s = 0.0;
    opts->warmup = 0.0;
    opts->cooldown = 0.0;
    opts->name = "bulk_topic";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;
    opts->reliable = false;

    return tt_example_parse_args(argc, argv, opts,
                                 TT_EXAMPLE_OPT_DURATION | TT_EXAMPLE_OPT_WARMUP_COOLDOWN | TT_EXAMPLE_OPT_RELIABLE);
}

int main(int argc, char** argv) {
    struct tt_example_cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    warmup_s = opts.warmup;
    cooldown_s = opts.cooldown;
    duration_s = opts.duration_s;

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

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BulkTopic, opts.name, (tt_SUBSCRIBER_CALLBACK)bulk_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }
    if (opts.reliable) {
        sub.reliable = true; // -R - see tt_Subscriber.reliable's own doc comment (tickle.h)
        printf("RELIABLE delivery\n");
    }

    uint64_t start_time = tt_get_ns();
    tt_Node_schedule(&node, start_time + tt_SECOND, report, NULL);
    // handle_duration_elapsed isn't scheduled here - see bulk_callback()'s own comment on why it
    // has to wait for the first real message instead. This safety net (see its own comment) is,
    // so a real -d still eventually bounds a completely dead run - +30s is arbitrary but generous:
    // real traffic normally arrives within a second or two of this process starting.
    if (opts.duration_s > 0.0) {
        const double no_traffic_margin_s = 30.0;
        tt_Node_schedule(&node, start_time + (uint64_t)((opts.duration_s + no_traffic_margin_s) * (double)tt_SECOND),
                         handle_no_traffic_timeout, NULL);
    }

    ret = tt_RET_OK;
    while (!g_exit_now && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
        // Checked here (right after poll() returns), not waited on elsewhere: this runs every
        // iteration regardless of how long until the next scheduled report()/finish_cooldown, so
        // Ctrl+C is caught right away rather than up to a second late.
        if (g_interrupted && !stopping) {
            begin_stopping(&node, tt_get_ns());
        }
    }

    print_summary(start_time);

    tt_Node_destroy(&node);

    return 0;
}
