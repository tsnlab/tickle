/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "liveliness_loss_detection" (rmw_tickle/COMPARISON.MD)
// - TickLE core native subscriber role. Real peer-departure detection in TickLE is node-level, not
// per-Publisher-entity (COMPARISON.MD's own native-QoS table, corrected this same pass, read
// directly from tickle.h): `tt_Node_set_discovery()` + `tt_DISCOVERY_CALLBACK` fires
// `departed=true` on `check_liveliness()`'s own fixed timeout window
// (`tt_LIVELINESS_MISS_THRESHOLD * tt_NODE_UPDATE_INTERVAL`, config.h, ~3s) - independent of
// whatever lease value `-T` announces on the wire (`sub.liveliness_lease_duration_ns` below is set
// purely for RxO-matching parity with the DDS twins' own QoS, it does not change this detection
// window at all). This is the real, expected, and worth-documenting difference from the DDS twins'
// own per-entity, lease-configurable detection: TickLE's own number here should track its fixed
// ~3s window regardless of `-T`, not the announced lease the way CycloneDDS/FastDDS's own results
// do.
//
// Detection latency measured entirely from this process's own clock (last-received-sample
// timestamp vs. departure-detected timestamp) - the same single-clock design principle the DDS
// twins' own scenario 8 established (never subtract a timestamp read on a different host's clock,
// e.g. the publisher's own `kill -9` wall-clock time).

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "../common/Bench.h"

static volatile sig_atomic_t g_interrupted = 0;
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static const double default_lease_s = 2.0;
static const double default_safety_cap_s = 40.0;
static const double safety_cap_buffer_s = 15.0;
static const double ns_per_ms = 1e6;

static uint64_t received = 0;
static uint64_t last_received_ns = 0;
static bool departed = false;
static uint64_t departed_detected_ns = 0;
static int64_t announce_age_at_detect_ns = -1;
// Arm B of the two-clock A/B is 215e16c4, which has no traffic clock at all, so the field the
// gap measurement reads does not exist in that build. Guarded with a -D rather than forked into a
// second harness source: a second harness binary would be exactly the confounder the A/B exists
// to rule out. Defaults on, so an ordinary build is unaffected.
#ifndef HAVE_TRAFFIC_CLOCK
#define HAVE_TRAFFIC_CLOCK 1
#endif

static int64_t gap_at_departure_ns = 0;

// Which node's departure was reported, and whether this run had ever seen a non-departure
// discovery event from it first. The discriminator for a contamination found at n=30: 12 of 90
// reps reported announce_age_at_detect_ms = 0.000, meaning the departure was detected in the same
// microsecond as the last UPDATE from that node - the signature of an announced goodbye rather
// than a timeout, although the client is killed with SIGKILL and cannot announce anything. The
// suspicion is the *previous* rep's server, shut down with SIGINT one second earlier, whose
// goodbye lands in this rep's socket. If so the node id will be one this run only just met and
// never received a plain discovery event from, which settles it either way without relying on the
// sweep's sequencing fix to work.
// The node whose departure this run is measuring, from -N. Everything else is ignored, loudly.
//
// Why this is not optional (2026-09-23, a real contamination, measured): 12 of 90 reps at n=30
// reported a departure that was not the client's. The rmw_tickle perf benchmark runs on the
// development box with TICKLE_NODE_ID 101, TickLE's compiled-in broadcast default is
// 255.255.255.255, and that box shares a layer-2 segment with both Pis' wlan0 - so node 101's
// clean shutdowns were arriving here and being recorded as this scenario's result. Taking the
// first departure of any node was the bug; the foreign traffic merely exposed it. A measurement
// that answers about whichever node happened to leave first is not a measurement.
static int watch_node_id = -1;
static uint64_t foreign_departures = 0;
static uint8_t foreign_node_ids[8];
static uint8_t foreign_node_count = 0;
static uint8_t departed_node_id = 0;
static bool departed_node_seen_before = false;
static uint64_t update_last_seen_at_detect_ns = 0;
// 256 entries because node_id is a uint8_t and tt_Node indexes its own per-node arrays the same
// way (tt_MAX_ENDPOINT_COUNT), so this is sized by the type rather than by a guess.
#define NODE_ID_COUNT 256
static bool node_seen[NODE_ID_COUNT];

static void stream_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct BenchData* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    (void)data;
    received++;
    last_received_ns = tt_get_ns();
}

static void discovery_callback(struct tt_Node* node, uint8_t node_id, uint32_t endpoint_id, uint8_t kind,
                               bool is_departed, void* param) {
    (void)endpoint_id;
    (void)kind;
    (void)param;
    if (!is_departed) {
        node_seen[node_id] = true;
        return;
    }
    if (watch_node_id >= 0 && node_id != (uint8_t)watch_node_id) {
        // Counted and named rather than silently skipped: if this scenario is ever run on a
        // contaminated network again, the RESULT line says so instead of looking clean.
        foreign_departures++;
        bool already = false;
        for (uint8_t i = 0; i < foreign_node_count; i++) {
            if (foreign_node_ids[i] == node_id) {
                already = true;
            }
        }
        if (!already && foreign_node_count < (uint8_t)(sizeof(foreign_node_ids) / sizeof(foreign_node_ids[0]))) {
            foreign_node_ids[foreign_node_count++] = node_id;
        }
        return;
    }
    if (!departed) {
        departed = true;
        departed_detected_ns = tt_get_ns();
        departed_node_id = node_id;
        departed_node_seen_before = node_seen[node_id];
        update_last_seen_at_detect_ns = node->update_last_seen[node_id];
        // The two-clock liveliness rule's own arithmetic predicts zero shift in detection timing
        // while (last_traffic - last_announce) stays under the guard (lease/2 on this path). That
        // is a prediction about a quantity nobody was measuring, so measure it: reporting the gap
        // next to the latency turns "inside the published baseline" into "inside it for the reason
        // the design says".
        //
        // Signed, because the first version of this line was unsigned on the reasoning that an
        // UPDATE refreshes both clocks so traffic can never be older than announce. It can, by a
        // few hundred ns: process_packet() stamps traffic_last_seen from a tt_get_ns() at the top
        // of the function and update_last_seen from a later one further in. Every rep of the first
        // 60-rep run reported 18446744073709.551 ms - 2^64 ns - which is what that underflow looks
        // like once it reaches a double, and it destroys the magnitude the field exists to report.
        // Anchored to the node UPDATE, which is the event that actually governs TickLE's lease.
        // This is the field to quote against the CycloneDDS/FastDDS numbers: their harnesses
        // measure from the last received sample, and in DDS a sample is itself what refreshes the
        // lease, so their reference point and their expiry point are the same event and cancel -
        // which is why those figures land within ~1ms of the lease. TickLE refreshes the lease
        // from the node-level UPDATE instead, so measuring from data (detect_latency_ms below)
        // leaves the phase between the two clocks in the number. Both fields are reported because
        // they answer different questions: detect_latency_ms is the honest end-to-end figure for
        // an application that only ever sees data, and this one is the cross-framework-comparable
        // figure for the lease mechanism itself.
        announce_age_at_detect_ns = (int64_t)departed_detected_ns - (int64_t)node->update_last_seen[node_id];

#if HAVE_TRAFFIC_CLOCK
        gap_at_departure_ns = (int64_t)node->traffic_last_seen[node_id] - (int64_t)node->update_last_seen[node_id];
#endif
    }
}

int main(int argc, char** argv) {
    double lease_s = default_lease_s;
    double safety_cap_s = default_safety_cap_s;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            lease_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            safety_cap_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc) {
            watch_node_id = atoi(argv[++i]);
        }
    }
    // +15s buffer - see deadline_miss_detection/server.c's own doc comment for the real bug this
    // avoids. This scenario is invoked manually (not via run_scenario.sh's shared $CLIENT_ARGS,
    // since it needs a custom kill -9 step), so it wasn't hit here, fixed preemptively for
    // consistency with every other scenario's own server.c.
    safety_cap_s += safety_cap_buffer_s;

    // real HIL link's own broadcast address - see best_effort_latency/server.c's own doc comment
    // for the real bug this avoids.
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    static struct tt_Discovery discovery = {0};
    tt_Node_set_discovery(&node, &discovery, discovery_callback, NULL);

    struct tt_Subscriber sub;
    ret = tt_Node_create_subscriber(&node, &sub, &BenchTopic, "stream", (tt_SUBSCRIBER_CALLBACK)stream_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }
    sub.liveliness_lease_duration_ns = (uint64_t)(lease_s * (double)tt_SECOND);

    uint64_t deadline = tt_get_ns() + (uint64_t)(safety_cap_s * (double)tt_SECOND);
    // 200ms - fine enough granularity to measure detection latency.
    const int64_t poll_timeout_ns = 200LL * 1000 * 1000;
    ret = tt_RET_OK;
    while (!g_interrupted && !departed && tt_get_ns() < deadline && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, poll_timeout_ns);
    }

    // Measured from the last received *data* sample, so its reference point is stale by however
    // long ago that sample was - up to one client publish interval. That staleness is what put a
    // ~500ms second mode in the first 60-rep run: at the old 0.5s interval the last packet before
    // the kill was either the UPDATE itself or a sample 500ms after it, and nothing in between.
    // The client now paces at 0.1s, matching the CycloneDDS and FastDDS twins, which narrows the
    // artifact to 100ms; announce_age_at_detect_ms above is the field that does not carry it.
    double detect_latency_ms =
        (departed && last_received_ns != 0) ? (double)(departed_detected_ns - last_received_ns) / ns_per_ms : -1.0;

// Eight ids at three digits plus separators, with room for the "none" case and a terminator.
#define FOREIGN_LIST_LEN 64
    char foreign_list[FOREIGN_LIST_LEN] = "none";
    if (foreign_node_count > 0) {
        int off = 0;
        for (uint8_t i = 0; i < foreign_node_count && off < (int)sizeof(foreign_list) - 8; i++) {
            off += snprintf(foreign_list + off, sizeof(foreign_list) - (size_t)off, i == 0 ? "%u" : ",%u",
                            (unsigned)foreign_node_ids[i]);
        }
    }

    printf("RESULT: framework=tickle scenario=liveliness_loss_detection role=server recv=%lu departed=%d "
           "detect_latency_ms=%.3f announce_age_at_detect_ms=%.3f gap_at_departure_ms=%.3f "
           "departed_node=%u node_seen_before=%d update_last_seen_ns=%lu detected_ns=%lu "
           "watch_node=%d foreign_departures=%lu foreign_nodes=%s\n",
           (unsigned long)received, departed, detect_latency_ms,
           departed ? (double)announce_age_at_detect_ns / ns_per_ms : -1.0, (double)gap_at_departure_ns / ns_per_ms,
           (unsigned)departed_node_id, departed_node_seen_before ? 1 : 0, (unsigned long)update_last_seen_at_detect_ns,
           (unsigned long)departed_detected_ns, watch_node_id, (unsigned long)foreign_departures, foreign_list);

    tt_Node_destroy(&node);
    return 0;
}
