/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The adaptive rate control's estimator (tt_LinkOptions.rate_control = tt_RATE_CONTROL_ADAPTIVE;
// examples/perf_hil/experiments/adaptive_pacing/DESIGN.md): from what comes back of a paced send - bytes delivered,
// and now and then a round trip time - it finds the rate the link delivers and keeps the queue short. Plain C with no
// dependency on the rest of the library, so a test can drive it with a synthetic link (tests/test_adaptive_pacing.c).
//
// One ap_pacer per LINK, however many readers are on it: the caller pools the readers' delivered bytes into one
// cumulative count and feeds every reader's round trip samples to the one min filter. All times are ns, all rates bits
// per second, all sizes bytes. Not thread-safe: the caller is the node's own thread.

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
    24000U // a delivery-rate sample spans at least this much delivered, so that a
           // unit more or less is a small part of it (the BDP is a few frames)
#define AP_MIN_ROUND_NS \
    1000000ULL                   // and a round is never shorter than this, whatever the round trip: on a loopback it is
                                 // microseconds, a round then holds a frame or two and its rate is noise
#define AP_MAX_ROUND_RTTS 8U     // but a round is never longer than this many RTprop
#define AP_PROBE_EVERY_ROUNDS 8U // and does so every this many rounds
#define AP_QUEUE_TARGET_MIN_NS 2000000ULL // queue-delay target: at least this, else RTprop / AP_QUEUE_TARGET_RTT_DIV
#define AP_QUEUE_TARGET_RTT_DIV 4U
#define AP_QUEUE_TARGET_UNITS \
    2U // and at least this many data units of send time at the pacing rate: a unit is the resolution of a sample
#define AP_QUEUE_TRIPS 2U                // queue-delay samples over the target in a row that trip the guard
#define AP_DROP_REPAIR_PCT 50U           // repairs / bytes sent over a round above this trip the rate-drop guard
#define AP_APP_LIMITED_PCT 80U           // a round that sent less than this of its allowance is application-limited
#define AP_IDLE_RESTART_NS 1000000000ULL // quiet this long: the next send starts in START at the remembered rate
#define AP_INFLIGHT_GAIN_PCT 200U        // data in flight is held to this x BtlBw x RTprop
#define AP_START_RATE_BPS 1000000ULL     // the rate a send starts at, clamped to [floor, cap], before any is measured
#define AP_NS_PER_SECOND 1000000000ULL   // time and rate arithmetic
#define AP_PERCENT 100U
#define AP_BITS_PER_BYTE 8U
#define AP_RECENT_RTTS 2U           // the queue guard reads the least of this many latest round trip samples
#define AP_SOLICIT_UNITS 2U         // solicitations are at least this many data units of send time apart
#define AP_STAMPS 256U              // send times kept, one per data unit sent: the round trip of a unit shown delivered
#define AP_STALL_NS 1000000000ULL   // sending this long with nothing delivered restarts the search
#define AP_BW_RESET_FRACTION 4U     // a guard trip lowers BtlBw to at most this fraction of it in one step
#define AP_MIN_INFLIGHT_BYTES 3000U // the least the in-flight cap is, two full frames and a little
#define AP_SEED_RTT_NS 10000000ULL  // RTprop until a first sample, when the caller has no seed

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
    uint64_t unit_bytes;  // a data unit on the wire: what the round trip samples resolve to, at the pacing rate
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
    uint32_t probe_gain_pct;             // this probe round's gain over BtlBw
    uint64_t probe_base_bps;             // the BtlBw a probe round started from
    bool draining;                       // the queue is over its target: run below the delivery rate until it is not
    uint32_t queue_over;                 // consecutive rounds over the queue target
    uint64_t recent_rtt[AP_RECENT_RTTS]; // the last round trip samples, newest overwriting oldest
    uint32_t recent_n;                   // samples taken, saturating at AP_RECENT_RTTS
    uint64_t units_sent;                 // data units sent, originals only (a repair is not a new unit)
    uint64_t top_units;           // data units shown received, or lost in front of a received one, pooled (cumulative)
    uint64_t stall_start_ns;      // when the current round began with nothing delivered, 0: not stalled
    uint64_t stamp_ns[AP_STAMPS]; // send time of unit n, at n % AP_STAMPS
    uint32_t low_rounds;          // consecutive rounds below half the BtlBw
    uint64_t last_active_ns;      // when this pacer last sent
    uint64_t rounds;              // active rounds so far, the max filter's clock
    // counters
    uint64_t samples;
    uint64_t queue_trips;
    uint64_t drop_trips;
    uint64_t stall_restarts;
};

static inline uint64_t ap_min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}
static inline uint64_t ap_max_u64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

static inline void ap_filter_init(struct ap_filter* flt, bool max, uint64_t bucket_len) {
    flt->max = max;
    flt->bucket_len = bucket_len < 1 ? 1 : bucket_len;
    flt->bucket_start = 0;
    flt->head = 0;
    for (uint32_t i = 0; i < AP_BUCKETS; i++) {
        flt->value[i] = max ? 0 : UINT64_MAX;
    }
}

// Empties the filter and puts `value` in, as of `now`.
static inline void ap_filter_reset(struct ap_filter* flt, uint64_t value, uint64_t now) {
    for (uint32_t i = 0; i < AP_BUCKETS; i++) {
        flt->value[i] = flt->max ? 0 : UINT64_MAX;
    }
    flt->head = 0;
    flt->bucket_start = now;
    flt->value[0] = value;
}

// Moves the filter's current bucket up to `now`, clearing the buckets it passes.
static inline void ap_filter_advance(struct ap_filter* flt, uint64_t now) {
    if (now < flt->bucket_start) {
        return;
    }
    uint64_t steps = (now - flt->bucket_start) / flt->bucket_len;
    if (steps == 0) {
        return;
    }
    if (steps >= AP_BUCKETS) {
        for (uint32_t i = 0; i < AP_BUCKETS; i++) {
            flt->value[i] = flt->max ? 0 : UINT64_MAX;
        }
        flt->head = 0;
    } else {
        for (uint64_t idx = 0; idx < steps; idx++) {
            flt->head = (flt->head + 1U) % AP_BUCKETS;
            flt->value[flt->head] = flt->max ? 0 : UINT64_MAX;
        }
    }
    flt->bucket_start += steps * flt->bucket_len;
}

static inline void ap_filter_put(struct ap_filter* flt, uint64_t value, uint64_t now) {
    ap_filter_advance(flt, now);
    uint64_t* slot = &flt->value[flt->head];
    *slot = flt->max ? ap_max_u64(*slot, value) : ap_min_u64(*slot, value);
}

// The filter's estimate; 0 for an empty max filter, UINT64_MAX for an empty min filter.
static inline uint64_t ap_filter_get(const struct ap_filter* flt) {
    uint64_t best = flt->max ? 0 : UINT64_MAX;
    for (uint32_t i = 0; i < AP_BUCKETS; i++) {
        best = flt->max ? ap_max_u64(best, flt->value[i]) : ap_min_u64(best, flt->value[i]);
    }
    return best;
}

static inline uint64_t ap_rtprop_ns(const struct ap_pacer* ctl) {
    uint64_t rtt = ap_filter_get(&ctl->rtt);
    return rtt == UINT64_MAX ? ctl->seed_rtt_ns : rtt;
}

static inline uint64_t ap_btlbw_bps(const struct ap_pacer* ctl) {
    return ap_filter_get(&ctl->bw);
}

static inline uint64_t ap_queue_target_ns(const struct ap_pacer* ctl) {
    if (ctl->queue_cfg_ns != 0) {
        return ctl->queue_cfg_ns;
    }
    uint64_t unit_ns = ctl->rate_bps != 0 ? (ctl->unit_bytes * AP_BITS_PER_BYTE * AP_NS_PER_SECOND) / ctl->rate_bps : 0;
    uint64_t target_ns = ap_max_u64(AP_QUEUE_TARGET_MIN_NS, ap_rtprop_ns(ctl) / AP_QUEUE_TARGET_RTT_DIV);
    return ap_max_u64(target_ns, AP_QUEUE_TARGET_UNITS * unit_ns);
}

static inline uint64_t ap_clamp_rate(const struct ap_pacer* ctl, uint64_t rate) {
    return ap_min_u64(ap_max_u64(rate, ctl->min_bps), ctl->cap_bps);
}

// seed_rtt_ns: a first round trip to the peer (tt_Context.announce_rtt_ns), 0 = AP_SEED_RTT_NS.
static inline void ap_init(struct ap_pacer* ctl, uint64_t min_bps, uint64_t cap_bps, uint64_t queue_cfg_ns,
                           uint64_t seed_rtt_ns, uint64_t unit_bytes) {
    *ctl = (struct ap_pacer) {0};
    ctl->min_bps = min_bps;
    ctl->cap_bps = cap_bps;
    ctl->queue_cfg_ns = queue_cfg_ns;
    ctl->unit_bytes = unit_bytes;
    ctl->seed_rtt_ns = seed_rtt_ns != 0 ? seed_rtt_ns : AP_SEED_RTT_NS;
    ap_filter_init(&ctl->bw, true, AP_BW_BUCKET_ROUNDS);
    ap_filter_init(&ctl->rtt, false, AP_RTT_WINDOW_NS / AP_BUCKETS);
    ctl->state = AP_START;
    ctl->probe_gain_pct = AP_PROBE_GAIN_PCT;
    ctl->rate_bps = ap_clamp_rate(ctl, AP_START_RATE_BPS);
}

// The rate to pace the link at, bits/s.
static inline uint64_t ap_rate_bps(const struct ap_pacer* ctl) {
    return ctl->rate_bps;
}

// How long a writer waits between solicitations (the HEARTBEATs that draw a report from a clean stream): the least of a
// round (the round trip, at least AP_MIN_ROUND_NS) and AP_SOLICIT_UNITS data units of send time at the pacing rate.
// Self-clocked at the report rate (a report asks the next solicitation) it sent a few thousand a second on a fast path,
// 156 bytes on the wire each, and the control traffic charged into the bucket took the whole rate of a slow start - the
// data starved, the estimate read nothing delivered, and the floor it fell to was less than the solicitations alone.
static inline uint64_t ap_solicit_gap_ns(const struct ap_pacer* ctl) {
    uint64_t unit_ns = ctl->rate_bps != 0 ? (ctl->unit_bytes * AP_BITS_PER_BYTE * AP_NS_PER_SECOND) / ctl->rate_bps : 0;
    uint64_t round_ns = ap_max_u64(ap_rtprop_ns(ctl), AP_MIN_ROUND_NS);
    return ap_max_u64(round_ns, AP_SOLICIT_UNITS * unit_ns);
}

// How many bytes may be in flight (sent, not yet delivered); UINT64_MAX until a rate is known.
static inline uint64_t ap_inflight_limit(const struct ap_pacer* ctl) {
    uint64_t bandwidth = ap_btlbw_bps(ctl);
    if (bandwidth == 0) {
        return UINT64_MAX;
    }
    uint64_t bdp = (bandwidth / AP_BITS_PER_BYTE) * ap_rtprop_ns(ctl) / AP_NS_PER_SECOND;
    return ap_max_u64(bdp * AP_INFLIGHT_GAIN_PCT / AP_PERCENT, AP_MIN_INFLIGHT_BYTES);
}

// A new round starts at `now`.
static inline void ap_round_begin(struct ap_pacer* ctl, uint64_t now) {
    ctl->round_start_ns = now;
    ctl->round_delivered = ctl->delivered;
    ctl->round_sent = 0;
    ctl->round_repair = 0;
    ctl->stall_start_ns = 0;
}

// Quiet for AP_IDLE_RESTART_NS: the remembered rate may be stale. The send starts again from it, keeping the filters
// (the max filter ages in active rounds, so idle time does not empty it). With a remembered rate it starts there in
// CRUISE and probes upward after its first round (a link that got slower meanwhile is caught by the queue guard);
// without one it starts in START.
static inline void ap_restart_after_idle(struct ap_pacer* ctl, uint64_t now) {
    uint64_t bandwidth = ap_btlbw_bps(ctl);
    ctl->state = bandwidth != 0 ? AP_CRUISE : AP_START;
    ctl->stalls = 0;
    ctl->prev_round_bps = 0;
    ctl->phase = AP_PH_CRUISE;
    ctl->draining = false;
    ctl->rounds_since_probe = AP_PROBE_EVERY_ROUNDS - 1U;
    ap_round_begin(ctl, now);
    ctl->rate_bps = ap_clamp_rate(ctl, bandwidth != 0 ? bandwidth : AP_START_RATE_BPS);
}

// The caller sent `bytes` (a repair or new data) at `now`.
static inline void ap_note_send(struct ap_pacer* ctl, uint64_t now, uint64_t bytes, bool repair) {
    if (ctl->last_active_ns != 0 && now - ctl->last_active_ns > AP_IDLE_RESTART_NS) {
        ap_restart_after_idle(ctl, now);
    }
    if (ctl->round_start_ns == 0) {
        ctl->round_start_ns = now;
        ctl->round_delivered = ctl->delivered;
    }
    ctl->last_active_ns = now;
    ctl->round_sent += bytes;
    if (repair) {
        ctl->round_repair += bytes;
    }
}

// `bytes` of what was sent in this round were repairs (a retransmission, counted into the bytes sent by
// ap_note_send()).
static inline void ap_note_repair(struct ap_pacer* ctl, uint64_t bytes) {
    ctl->round_repair += bytes;
}

// A data unit (an original send, not a repair) left at `now`: its send time is kept, so that the round trip of a unit a
// reader later shows received can be read off it - the only round trip sample, since a report cannot be matched to the
// solicitation it answers, and one sent for another reason would read as a round trip shorter than the path.
static inline void ap_note_unit_sent(struct ap_pacer* ctl, uint64_t now) {
    ctl->stamp_ns[ctl->units_sent % AP_STAMPS] = now;
    ctl->units_sent++;
}

// One finished round, as the estimator reads it.
struct ap_round {
    uint64_t sample_bps; // delivered over the round, bits per second
    bool app_limited;    // the pacer sent less than AP_APP_LIMITED_PCT of its allowance
    bool idle;           // it sent nothing
};

// The rate-drop guard: most of what was sent was a repair, or the link delivers under half of what it did, twice.
// Returns the BtlBw to go on with: the round's own sample after a trip - at least a quarter of what it was, so that one
// noisy round cannot take the estimate to nothing - else `bandwidth`.
static inline uint64_t ap_drop_guard(struct ap_pacer* ctl, const struct ap_round* round, uint64_t bandwidth) {
    if (round->idle) {
        ctl->low_rounds = 0;
        return bandwidth;
    }
    bool drop = ctl->round_repair * AP_PERCENT > ctl->round_sent * AP_DROP_REPAIR_PCT && ctl->state == AP_CRUISE;
    if (!round->app_limited && bandwidth != 0 && round->sample_bps * 2U < bandwidth) {
        ctl->low_rounds++;
        drop = drop || ctl->low_rounds >= 2U;
    } else {
        ctl->low_rounds = 0;
    }
    if (!drop) {
        return bandwidth;
    }
    ctl->drop_trips++;
    uint64_t lowered = ap_max_u64(round->sample_bps, bandwidth / AP_BW_RESET_FRACTION);
    ap_filter_reset(&ctl->bw, lowered, ctl->rounds);
    ctl->low_rounds = 0;
    ctl->phase = AP_PH_CRUISE;
    ctl->rounds_since_probe = 0;
    return lowered;
}

// Search again from the start rate: nothing is known of the link.
static inline void ap_restart_search(struct ap_pacer* ctl, uint64_t now) {
    ctl->stall_restarts++;
    ap_filter_reset(&ctl->bw, 0, ctl->rounds);
    ctl->state = AP_START;
    ctl->stalls = 0;
    ctl->prev_round_bps = 0;
    ctl->phase = AP_PH_CRUISE;
    ctl->draining = false;
    ctl->low_rounds = 0;
    ctl->queue_over = 0;
    ctl->rate_bps = ap_clamp_rate(ctl, AP_START_RATE_BPS);
    ap_round_begin(ctl, now);
}

// START: the rate doubles while the delivery rate still grows, and BtlBw is found when it stops growing.
static inline void ap_start_round(struct ap_pacer* ctl, const struct ap_round* round, uint64_t bandwidth) {
    if (round->idle) {
        return;
    }
    bool grew = ctl->prev_round_bps == 0 || round->sample_bps * AP_PERCENT >= ctl->prev_round_bps * AP_START_GROWTH_PCT;
    if (round->app_limited) {
        grew = true; // not the link's limit: no verdict yet
    }
    ctl->stalls = grew ? 0U : ctl->stalls + 1U;
    ctl->prev_round_bps = round->sample_bps;
    if (ctl->stalls >= AP_START_STALL_ROUNDS) {
        ctl->state = AP_CRUISE;
        ctl->rate_bps = ap_clamp_rate(ctl, bandwidth);
        ctl->rounds_since_probe = 0;
    } else {
        ctl->rate_bps = ap_clamp_rate(ctl, ctl->rate_bps * 2U);
    }
}

// CRUISE: BtlBw, a probe round now and then, and a drain round after a probe that found nothing.
static inline void ap_cruise_round(struct ap_pacer* ctl, const struct ap_round* round, uint64_t bandwidth) {
    ctl->rounds_since_probe++;
    if (ctl->phase == AP_PH_PROBE) {
        // A probe that raised the delivery rate is followed by another at once (the link got faster); one that did
        // not is followed by a drain round for whatever queue it made.
        if (round->sample_bps * AP_PERCENT >= ctl->probe_base_bps * AP_PROBE_RISE_PCT) {
            ctl->probe_base_bps = bandwidth;
            ctl->probe_gain_pct = AP_PROBE_RUN_GAIN_PCT;
        } else {
            ctl->phase = AP_PH_DRAIN;
            ctl->probe_gain_pct = AP_PROBE_GAIN_PCT;
        }
    } else if (ctl->phase == AP_PH_DRAIN) {
        ctl->phase = AP_PH_CRUISE;
        ctl->rounds_since_probe = 0;
    } else if (ctl->rounds_since_probe >= AP_PROBE_EVERY_ROUNDS && !ctl->draining && !round->app_limited) {
        ctl->phase = AP_PH_PROBE;
        ctl->probe_base_bps = bandwidth;
        ctl->probe_gain_pct = AP_PROBE_GAIN_PCT;
    }
    uint64_t rate = bandwidth;
    if (ctl->draining || ctl->phase == AP_PH_DRAIN) {
        rate = bandwidth * AP_DRAIN_GAIN_PCT / AP_PERCENT;
    } else if (ctl->phase == AP_PH_PROBE) {
        rate = bandwidth * ctl->probe_gain_pct / AP_PERCENT;
    }
    ctl->rate_bps = ap_clamp_rate(ctl, rate);
}

// What the round delivered, in bits per second, over `elapsed` ns.
static inline uint64_t ap_delivery_rate_bps(const struct ap_pacer* ctl, uint64_t elapsed) {
    uint64_t bytes = ctl->delivered - ctl->round_delivered;
    return elapsed != 0 ? (bytes * AP_BITS_PER_BYTE * AP_NS_PER_SECOND) / elapsed : 0;
}

// The queue guard, at each round trip sample: the least of the latest AP_RECENT_RTTS samples over the least ever seen
// is the queue. (One sample is not read: a unit shown delivered by a repair that closed a gap was sent long before it
// was shown, and reads as a long round trip; the least of a few does not.) Over the target AP_QUEUE_TRIPS evaluations
// in a row: the real-time term - stop probing, take the rate the link is delivering now instead of the older maximum,
// and drain below it.
static inline void ap_queue_guard(struct ap_pacer* ctl, uint64_t now) {
    if (ctl->recent_n < AP_RECENT_RTTS) {
        return;
    }
    uint64_t least_recent = UINT64_MAX;
    for (uint32_t idx = 0; idx < AP_RECENT_RTTS; idx++) {
        least_recent = ap_min_u64(least_recent, ctl->recent_rtt[idx]);
    }
    uint64_t floor_ns = ap_rtprop_ns(ctl);
    uint64_t target_ns = ap_queue_target_ns(ctl);
    uint64_t queue_ns = least_recent > floor_ns ? least_recent - floor_ns : 0;
    if (queue_ns <= target_ns) {
        ctl->queue_over = 0;
        if (queue_ns < target_ns / 2U) {
            ctl->draining = false; // the queue is back under half its target
        }
        return;
    }
    ctl->queue_over++;
    if (ctl->queue_over < AP_QUEUE_TRIPS) {
        return;
    }
    ctl->queue_trips++;
    ctl->queue_over = 0;
    ctl->phase = AP_PH_CRUISE;
    ctl->draining = true;
    if (ctl->state == AP_START) {
        ctl->state = AP_CRUISE;
    }
    uint64_t elapsed = now - ctl->round_start_ns;
    uint64_t sample = ap_delivery_rate_bps(ctl, elapsed);
    uint64_t bandwidth = ap_btlbw_bps(ctl);
    if (sample != 0 && sample < bandwidth && elapsed >= ap_rtprop_ns(ctl) / 2U) {
        ap_filter_reset(&ctl->bw, ap_max_u64(sample, bandwidth / AP_BW_RESET_FRACTION), ctl->rounds);
        bandwidth = ap_btlbw_bps(ctl);
    }
    ctl->rate_bps = ap_clamp_rate(ctl, (bandwidth != 0 ? bandwidth : ctl->rate_bps) * AP_DRAIN_GAIN_PCT / AP_PERCENT);
    ctl->rounds_since_probe = 0;
}

static inline void ap_end_round(struct ap_pacer* ctl, uint64_t now) {
    uint64_t elapsed = now - ctl->round_start_ns;
    uint64_t allowance = (ctl->rate_bps / AP_BITS_PER_BYTE) * elapsed / AP_NS_PER_SECOND;
    struct ap_round round = {.sample_bps = ap_delivery_rate_bps(ctl, elapsed),
                             .app_limited = ctl->round_sent * AP_PERCENT < allowance * AP_APP_LIMITED_PCT,
                             .idle = ctl->round_sent == 0};
    uint64_t bandwidth = ap_btlbw_bps(ctl);
    ctl->samples++;
    if (!round.idle) {
        ctl->rounds++;
        if (!round.app_limited || round.sample_bps > bandwidth) {
            ap_filter_put(&ctl->bw, round.sample_bps, ctl->rounds);
        } else {
            ap_filter_advance(&ctl->bw, ctl->rounds);
        }
        bandwidth = ap_btlbw_bps(ctl);
    }
    bandwidth = ap_drop_guard(ctl, &round, bandwidth);
    if (ctl->state == AP_START) {
        ap_start_round(ctl, &round, bandwidth);
    } else if (bandwidth == 0 && !round.idle) {
        ap_restart_search(ctl, now); // CRUISE on an estimate of nothing: never sit at the floor
        return;
    } else {
        ap_cruise_round(ctl, &round, bandwidth);
    }
    ap_round_begin(ctl, now);
}

// A round trip sample from the newest unit a report shows received (pooled over the readers): the report cannot have
// been sent before that unit arrived, so now less the unit's send time is never less than the path's round trip.
// top_units counts the units held and the units missing below a held one: a unit lost on the way would otherwise leave
// the count behind the sends by the losses, and every sample longer by their send time.
static inline void ap_note_top_units(struct ap_pacer* ctl, uint64_t now, uint64_t top_units) {
    if (top_units <= ctl->top_units) {
        return;
    }
    uint64_t idx = top_units - 1U;
    ctl->top_units = top_units;
    if (idx >= ctl->units_sent || ctl->units_sent - idx > AP_STAMPS) {
        return; // a unit that was not sent as one (a small sample's), or too long ago to have a stamp
    }
    uint64_t sent_ns = ctl->stamp_ns[idx % AP_STAMPS];
    if (now <= sent_ns) {
        return;
    }
    uint64_t rtt_ns = now - sent_ns;
    ap_filter_put(&ctl->rtt, rtt_ns, now);
    ctl->recent_rtt[ctl->recent_n % AP_RECENT_RTTS] = rtt_ns;
    ctl->recent_n++;
    ap_queue_guard(ctl, now);
}

// Feedback at `now`: the readers have `delivered` bytes and `top_units` units in all (cumulative, pooled over the
// link's readers, never less than before).
static inline void ap_on_feedback(struct ap_pacer* ctl, uint64_t now, uint64_t delivered, uint64_t top_units) {
    if (delivered > ctl->delivered) {
        ctl->delivered = delivered;
    }
    if (ctl->round_start_ns == 0) {
        ctl->round_start_ns = now;
        ctl->round_delivered = ctl->delivered;
    }
    ap_note_top_units(ctl, now, top_units);
    uint64_t elapsed = now - ctl->round_start_ns;
    uint64_t rtprop = ap_rtprop_ns(ctl);
    uint64_t least_ns = ap_max_u64(rtprop, AP_MIN_ROUND_NS);
    uint64_t bytes = ctl->delivered - ctl->round_delivered;
    if (bytes == 0) {
        // Nothing delivered: no sample - a round with no news says nothing of the link. Sending for AP_STALL_NS with
        // none is the link gone or the estimate wrong: search again.
        if (ctl->round_sent != 0 && elapsed >= AP_STALL_NS) {
            ap_restart_search(ctl, now);
        }
        return;
    }
    if (elapsed >= least_ns && (bytes >= AP_MIN_SAMPLE_BYTES || elapsed >= AP_MAX_ROUND_RTTS * least_ns)) {
        ap_end_round(ctl, now);
    }
}

#endif // TT_PACING_ADAPTIVE_H
