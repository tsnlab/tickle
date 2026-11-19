/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The adaptive send pacer's estimator (examples/perf_hil/experiments/adaptive_pacing/DESIGN.md): from what comes back
// of a paced send - bytes delivered, and now and then a round trip time - it finds the rate the link delivers and
// keeps the queue short. Plain C with no dependency on the rest of the library, so a test can drive it with a
// synthetic link (tests/test_adaptive_pacing.c). Nothing in the library includes it yet.
//
// One ap_pacer per link. All times are ns, all rates bits per second, all sizes bytes. Not thread-safe: the caller
// is the node's own thread.

#ifndef TT_PACING_ADAPTIVE_H
#define TT_PACING_ADAPTIVE_H

#include <stdbool.h>
#include <stdint.h>

// (initial) constants: starting choices, to be fixed by the model and the tc runs, not results.
#define AP_BUCKETS 8U                   // buckets in a windowed filter
#define AP_BW_BUCKET_ROUNDS 1U          // the delivery-rate max filter: one bucket per active round, AP_BUCKETS rounds
#define AP_RTT_WINDOW_NS 10000000000ULL // the round-trip min filter spans this long of wall time
#define AP_START_GROWTH_PCT 125U        // a START round grows the delivery rate by at least this, else it is a stall
#define AP_START_STALL_ROUNDS 3U        // stalls in a row that end START
#define AP_PROBE_GAIN_PCT 125U          // CRUISE probes at this x BtlBw for one round, then drains at the next
#define AP_PROBE_RUN_GAIN_PCT 200U      // the next probe after one that raised the delivery rate: the link got faster
#define AP_DRAIN_GAIN_PCT 75U           // the round after a probe, and while the queue is over its target
#define AP_PROBE_RISE_PCT 110U          // a probe that raised the delivery rate by this much is followed by another
#define AP_MIN_SAMPLE_BYTES \
    24000U                                // a delivery-rate sample spans at least this much delivered, so that a
                                          // unit more or less is a small part of it (the BDP is a few frames)
#define AP_MAX_ROUND_RTTS 8U              // but a round is never longer than this many RTprop
#define AP_PROBE_EVERY_ROUNDS 8U          // and does so every this many rounds
#define AP_QUEUE_TARGET_MIN_NS 2000000ULL // queue-delay target: at least this, else RTprop / AP_QUEUE_TARGET_RTT_DIV
#define AP_QUEUE_TARGET_RTT_DIV 4U
#define AP_QUEUE_TRIPS 2U                // queue-delay samples over the target in a row that trip the guard
#define AP_DROP_REPAIR_PCT 50U           // repairs / bytes sent over a round above this trip the rate-drop guard
#define AP_APP_LIMITED_PCT 80U           // a round that sent less than this of its allowance is application-limited
#define AP_IDLE_RESTART_NS 1000000000ULL // quiet this long: the next send starts in START at the remembered rate
#define AP_INFLIGHT_GAIN_PCT 200U        // data in flight is held to this x BtlBw x RTprop
#define AP_SEED_RTT_NS 10000000ULL       // RTprop until a first sample, when the caller has no seed

struct ap_filter {
    uint64_t value[AP_BUCKETS];
    uint64_t bucket_len; // in the filter's own clock: ns for the min filter, active rounds for the max filter
    uint64_t bucket_start;
    uint32_t head;
    bool max; // max filter (empty = 0), else min filter (empty = UINT64_MAX)
};

enum ap_state { AP_START, AP_CRUISE };
enum ap_phase { AP_PH_CRUISE, AP_PH_PROBE, AP_PH_DRAIN };

struct ap_pacer {
    uint64_t cap_bps; // never above (the link's link_rate or the user's pacing_rate)
    uint64_t min_bps; // never below
    uint64_t rate_bps;
    uint64_t queue_cfg_ns; // 0 = max(AP_QUEUE_TARGET_MIN_NS, RTprop / AP_QUEUE_TARGET_RTT_DIV)
    uint64_t seed_rtt_ns;
    struct ap_filter bw;  // delivery rate, max over active rounds (idle time does not age it)
    struct ap_filter rtt; // round trip, min over wall time
    enum ap_state state;
    uint64_t round_start_ns;
    uint64_t round_delivered; // cumulative delivered bytes at the round's start
    uint64_t delivered;       // cumulative delivered bytes, as last reported
    uint64_t round_sent;      // bytes sent in this round
    uint64_t round_repair;    // of them, repairs
    uint64_t prev_round_bps;  // START: the last round's delivery rate
    uint32_t stalls;
    uint32_t rounds_since_probe;
    enum ap_phase phase;
    uint32_t probe_gain_pct; // this probe round's gain over BtlBw
    uint64_t probe_base_bps; // the BtlBw a probe round started from
    bool draining;           // the queue is over its target: run below the delivery rate until it is not
    uint32_t queue_over;     // consecutive over-target samples
    uint32_t low_rounds;     // consecutive rounds below half the BtlBw
    uint64_t last_active_ns; // when this pacer last sent
    uint64_t rounds;         // active rounds so far, the max filter's clock
    // counters
    uint64_t samples;
    uint64_t queue_trips;
    uint64_t drop_trips;
};

static inline uint64_t ap_min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}
static inline uint64_t ap_max_u64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

static inline void ap_filter_init(struct ap_filter* f, bool max, uint64_t bucket_len) {
    f->max = max;
    f->bucket_len = bucket_len < 1 ? 1 : bucket_len;
    f->bucket_start = 0;
    f->head = 0;
    for (uint32_t i = 0; i < AP_BUCKETS; i++) {
        f->value[i] = max ? 0 : UINT64_MAX;
    }
}

// Empties the filter and puts `value` in, as of `now`.
static inline void ap_filter_reset(struct ap_filter* f, uint64_t value, uint64_t now) {
    for (uint32_t i = 0; i < AP_BUCKETS; i++) {
        f->value[i] = f->max ? 0 : UINT64_MAX;
    }
    f->head = 0;
    f->bucket_start = now;
    f->value[0] = value;
}

// Moves the filter's current bucket up to `now`, clearing the buckets it passes.
static inline void ap_filter_advance(struct ap_filter* f, uint64_t now) {
    if (now < f->bucket_start) {
        return;
    }
    uint64_t steps = (now - f->bucket_start) / f->bucket_len;
    if (steps == 0) {
        return;
    }
    if (steps >= AP_BUCKETS) {
        for (uint32_t i = 0; i < AP_BUCKETS; i++) {
            f->value[i] = f->max ? 0 : UINT64_MAX;
        }
        f->head = 0;
    } else {
        for (uint64_t s = 0; s < steps; s++) {
            f->head = (f->head + 1U) % AP_BUCKETS;
            f->value[f->head] = f->max ? 0 : UINT64_MAX;
        }
    }
    f->bucket_start += steps * f->bucket_len;
}

static inline void ap_filter_put(struct ap_filter* f, uint64_t value, uint64_t now) {
    ap_filter_advance(f, now);
    uint64_t* slot = &f->value[f->head];
    *slot = f->max ? ap_max_u64(*slot, value) : ap_min_u64(*slot, value);
}

// The filter's estimate; 0 for an empty max filter, UINT64_MAX for an empty min filter.
static inline uint64_t ap_filter_get(const struct ap_filter* f) {
    uint64_t best = f->max ? 0 : UINT64_MAX;
    for (uint32_t i = 0; i < AP_BUCKETS; i++) {
        best = f->max ? ap_max_u64(best, f->value[i]) : ap_min_u64(best, f->value[i]);
    }
    return best;
}

static inline uint64_t ap_rtprop_ns(const struct ap_pacer* ap) {
    uint64_t rtt = ap_filter_get(&ap->rtt);
    return rtt == UINT64_MAX ? ap->seed_rtt_ns : rtt;
}

static inline uint64_t ap_btlbw_bps(const struct ap_pacer* ap) {
    return ap_filter_get(&ap->bw);
}

static inline uint64_t ap_queue_target_ns(const struct ap_pacer* ap) {
    if (ap->queue_cfg_ns != 0) {
        return ap->queue_cfg_ns;
    }
    return ap_max_u64(AP_QUEUE_TARGET_MIN_NS, ap_rtprop_ns(ap) / AP_QUEUE_TARGET_RTT_DIV);
}

static inline uint64_t ap_clamp_rate(const struct ap_pacer* ap, uint64_t rate) {
    return ap_min_u64(ap_max_u64(rate, ap->min_bps), ap->cap_bps);
}

// seed_rtt_ns: a first round trip to the peer (tt_Context.announce_rtt_ns), 0 = AP_SEED_RTT_NS.
static inline void ap_init(struct ap_pacer* ap, uint64_t min_bps, uint64_t cap_bps, uint64_t queue_cfg_ns,
                           uint64_t seed_rtt_ns) {
    *ap = (struct ap_pacer) {0};
    ap->min_bps = min_bps;
    ap->cap_bps = cap_bps;
    ap->queue_cfg_ns = queue_cfg_ns;
    ap->seed_rtt_ns = seed_rtt_ns != 0 ? seed_rtt_ns : AP_SEED_RTT_NS;
    ap_filter_init(&ap->bw, true, AP_BW_BUCKET_ROUNDS);
    ap_filter_init(&ap->rtt, false, AP_RTT_WINDOW_NS / AP_BUCKETS);
    ap->state = AP_START;
    ap->probe_gain_pct = AP_PROBE_GAIN_PCT;
    ap->rate_bps = ap_clamp_rate(ap, min_bps);
}

// The rate to pace the link at, bits/s.
static inline uint64_t ap_rate_bps(const struct ap_pacer* ap) {
    return ap->rate_bps;
}

// How many bytes may be in flight (sent, not yet delivered); UINT64_MAX until a rate is known.
static inline uint64_t ap_inflight_limit(const struct ap_pacer* ap) {
    uint64_t bw = ap_btlbw_bps(ap);
    if (bw == 0) {
        return UINT64_MAX;
    }
    uint64_t bdp = (bw / 8U) * ap_rtprop_ns(ap) / 1000000000ULL;
    return ap_max_u64(bdp * AP_INFLIGHT_GAIN_PCT / 100U, 3000U);
}

// The caller sent `bytes` (a repair or new data) at `now`.
static inline void ap_note_send(struct ap_pacer* ap, uint64_t now, uint64_t bytes, bool repair) {
    if (ap->last_active_ns != 0 && now - ap->last_active_ns > AP_IDLE_RESTART_NS) {
        // Quiet: the remembered rate may be stale. Start again from it, keeping the filters (the max filter ages in
        // active rounds, so idle time does not empty it).
        // With a remembered rate the send starts there in CRUISE and probes upward after its first round (a
        // link that got slower meanwhile is caught by the queue guard); without one it starts in START.
        uint64_t bw = ap_btlbw_bps(ap);
        ap->state = bw != 0 ? AP_CRUISE : AP_START;
        ap->stalls = 0;
        ap->prev_round_bps = 0;
        ap->phase = AP_PH_CRUISE;
        ap->draining = false;
        ap->rounds_since_probe = AP_PROBE_EVERY_ROUNDS - 1U;
        ap->round_start_ns = now;
        ap->round_delivered = ap->delivered;
        ap->round_sent = 0;
        ap->round_repair = 0;
        ap->rate_bps = ap_clamp_rate(ap, bw != 0 ? bw : ap->min_bps);
    }
    if (ap->round_start_ns == 0) {
        ap->round_start_ns = now;
        ap->round_delivered = ap->delivered;
    }
    ap->last_active_ns = now;
    ap->round_sent += bytes;
    if (repair) {
        ap->round_repair += bytes;
    }
}

static inline void ap_end_round(struct ap_pacer* ap, uint64_t now) {
    uint64_t elapsed = now - ap->round_start_ns;
    uint64_t bytes = ap->delivered - ap->round_delivered;
    uint64_t sample = elapsed != 0 ? (bytes * 8ULL * 1000000000ULL) / elapsed : 0;
    uint64_t allowance = (ap->rate_bps / 8U) * elapsed / 1000000000ULL;
    bool app_limited = ap->round_sent * 100U < allowance * AP_APP_LIMITED_PCT;
    bool idle = ap->round_sent == 0;
    uint64_t bw = ap_btlbw_bps(ap);
    ap->samples++;

    if (!idle) {
        ap->rounds++;
        if (!app_limited || sample > bw) {
            ap_filter_put(&ap->bw, sample, ap->rounds);
        } else {
            ap_filter_advance(&ap->bw, ap->rounds);
        }
        bw = ap_btlbw_bps(ap);
    }

    // The rate-drop guard: most of what was sent was a repair, or the link delivers under half of what it did.
    bool drop = false;
    if (!idle && ap->round_repair * 100U > ap->round_sent * AP_DROP_REPAIR_PCT && ap->state == AP_CRUISE) {
        drop = true;
    }
    if (!idle && !app_limited && bw != 0 && sample * 2U < bw) {
        ap->low_rounds++;
        drop = drop || ap->low_rounds >= 2U;
    } else {
        ap->low_rounds = 0;
    }
    if (drop) {
        ap->drop_trips++;
        ap_filter_reset(&ap->bw, sample, ap->rounds);
        ap->low_rounds = 0;
        bw = sample;
        ap->phase = AP_PH_CRUISE;
        ap->rounds_since_probe = 0;
    }

    if (ap->state == AP_START) {
        if (!idle) {
            bool grew = ap->prev_round_bps == 0 || sample * 100U >= ap->prev_round_bps * AP_START_GROWTH_PCT;
            if (app_limited) {
                grew = true; // not the link's limit: no verdict yet
            }
            ap->stalls = grew ? 0U : ap->stalls + 1U;
            ap->prev_round_bps = sample;
            if (ap->stalls >= AP_START_STALL_ROUNDS) {
                ap->state = AP_CRUISE;
                ap->rate_bps = ap_clamp_rate(ap, bw);
                ap->rounds_since_probe = 0;
            } else {
                ap->rate_bps = ap_clamp_rate(ap, ap->rate_bps * 2U);
            }
        }
    } else {
        ap->rounds_since_probe++;
        if (ap->phase == AP_PH_PROBE) {
            // A probe that raised the delivery rate is followed by another at once (the link got faster); one
            // that did not is followed by a drain round for whatever queue it made.
            if (sample * 100U >= ap->probe_base_bps * AP_PROBE_RISE_PCT) {
                ap->probe_base_bps = bw;
                ap->probe_gain_pct = AP_PROBE_RUN_GAIN_PCT;
            } else {
                ap->phase = AP_PH_DRAIN;
                ap->probe_gain_pct = AP_PROBE_GAIN_PCT;
            }
        } else if (ap->phase == AP_PH_DRAIN) {
            ap->phase = AP_PH_CRUISE;
            ap->rounds_since_probe = 0;
        } else if (ap->rounds_since_probe >= AP_PROBE_EVERY_ROUNDS && !ap->draining && !app_limited) {
            ap->phase = AP_PH_PROBE;
            ap->probe_base_bps = bw;
            ap->probe_gain_pct = AP_PROBE_GAIN_PCT;
        }
        uint64_t rate = bw;
        if (ap->draining || ap->phase == AP_PH_DRAIN) {
            rate = bw * AP_DRAIN_GAIN_PCT / 100U;
        } else if (ap->phase == AP_PH_PROBE) {
            rate = bw * ap->probe_gain_pct / 100U;
        }
        ap->rate_bps = ap_clamp_rate(ap, rate);
    }

    ap->round_start_ns = now;
    ap->round_delivered = ap->delivered;
    ap->round_sent = 0;
    ap->round_repair = 0;
}

// Feedback at `now`: the reader has `delivered` bytes in all (cumulative, never less than before), and rtt_ns is a
// round trip time sample for this feedback, 0 if it has none.
static inline void ap_on_feedback(struct ap_pacer* ap, uint64_t now, uint64_t delivered, uint64_t rtt_ns) {
    if (delivered > ap->delivered) {
        ap->delivered = delivered;
    }
    if (ap->round_start_ns == 0) {
        ap->round_start_ns = now;
        ap->round_delivered = ap->delivered;
    }
    if (rtt_ns != 0) {
        ap_filter_put(&ap->rtt, rtt_ns, now);
        uint64_t floor_ns = ap_rtprop_ns(ap);
        if (rtt_ns > floor_ns && rtt_ns - floor_ns > ap_queue_target_ns(ap)) {
            ap->queue_over++;
            if (ap->queue_over >= AP_QUEUE_TRIPS) {
                // The real-time term: the queue is longer than the target. Stop probing, and take the rate the link
                // is delivering now instead of the older maximum.
                ap->queue_trips++;
                ap->queue_over = 0;
                ap->phase = AP_PH_CRUISE;
                ap->draining = true;
                if (ap->state == AP_START) {
                    ap->state = AP_CRUISE;
                }
                uint64_t elapsed = now - ap->round_start_ns;
                uint64_t sample =
                    elapsed != 0 ? ((ap->delivered - ap->round_delivered) * 8ULL * 1000000000ULL) / elapsed : 0;
                uint64_t bw = ap_btlbw_bps(ap);
                if (sample != 0 && sample < bw && elapsed >= ap_rtprop_ns(ap) / 2U) {
                    ap_filter_reset(&ap->bw, sample, ap->rounds);
                    bw = sample;
                }
                ap->rate_bps = ap_clamp_rate(ap, (bw != 0 ? bw : ap->rate_bps) * AP_DRAIN_GAIN_PCT / 100U);
                ap->rounds_since_probe = 0;
            }
        } else {
            ap->queue_over = 0;
            if (rtt_ns <= floor_ns || rtt_ns - floor_ns < ap_queue_target_ns(ap) / 2U) {
                ap->draining = false; // the queue is back under half its target
            }
        }
    }
    uint64_t elapsed = now - ap->round_start_ns;
    uint64_t rtprop = ap_rtprop_ns(ap);
    if (elapsed >= rtprop &&
        (ap->delivered - ap->round_delivered >= AP_MIN_SAMPLE_BYTES || elapsed >= AP_MAX_ROUND_RTTS * rtprop)) {
        ap_end_round(ap, now);
    }
}

#endif // TT_PACING_ADAPTIVE_H
