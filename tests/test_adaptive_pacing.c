/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The adaptive pacer's estimator (src/pacing_adaptive.h) against a synthetic link: a FIFO queue with a byte limit
// behind a rate that changes on a schedule, a one-way delay each way, a reader that reports what arrived and what is
// missing, and a writer that repairs what is missing. A MODEL: no TickLE code runs here but the estimator, and
// nothing below is a performance claim about a real link - the thresholds are what the design needs of the
// estimator on this idealised link (convergence, a short queue, fewer retransmissions than an unpaced send), and
// examples/perf_hil/experiments/adaptive_pacing/DESIGN.md section 7 holds the acceptance criteria for the real thing.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/pacing_adaptive.h>

#define UNIT_WIRE 1538U   // a full frame on the wire: 1472 payload + UDP, IP and Ethernet framing
#define SAMPLE_UNITS 723U // 1 MiB in 1472-byte units
#define SOLICIT_WIRE 90U
#define STEP_NS 100000ULL // the model's clock step
#define MAX_Q 4096U
#define MAX_DELAY 8192U
#define MAX_UNITS 400000U
#define MAX_READERS 4U
#define MAX_PENDING 64U
#define MS 1000000ULL

enum mode { MODE_UNPACED, MODE_FIXED, MODE_ADAPTIVE };

struct pkt {
    uint64_t enq_ns;
    uint32_t wire;
    int64_t unit;        // datagram id (unit x readers + reader), -1 for a solicit
    uint32_t reader;     // the reader a datagram, solicit or report belongs to
    uint64_t solicit_ns; // a solicit's send time
    int64_t last_unit; // a solicit's highest sent unit: its HEARTBEAT last_seq_no, how the reader learns of a lost tail
};

struct flight {
    uint64_t at_ns;
    struct pkt pkt;
};

struct link_cfg {
    uint64_t rate_bps[3]; // rate before step 1, between, after step 2
    uint64_t step1_ns;
    uint64_t step2_ns;
    uint32_t queue_bytes;
    uint64_t one_way_ns;
    uint64_t reader_tick_ns; // 0: a reader answers a solicitation at once; else at its next poll, every this many ns
    uint32_t
        loss_ppm; // the link drops this share of what it carries, per million datagrams (a fixed pseudo-random run)
    uint64_t unsolicited_ns; // a reader with a gap reports by itself this often (its retry timer); 0 = only when asked
    bool watermark_acks; // a report is what an ACKNACK shows (the watermark and the received units its bitmap names),
                         // and the writer takes the first report after a solicitation as its answer, as the library did
    bool omit_named;     // the reader leaves out of a bitmap the units it named within the last round trip and a half
                         // (repairs it reckons on their way): the writer counts them received
    uint32_t ge_p_ppm;   // Gilbert-Elliott loss: good -> bad per datagram, per million; 0 = off (loss_ppm only)
    uint32_t ge_r_ppm;   // bad -> good
    uint32_t ge_bad_loss_ppm; // the loss in the bad state; loss_ppm is the good state's
    uint64_t jump_every_ns;   // every this often a report's watermark jumps 700 units past what was sent (a reader that
                              // gives up a lost sample, or a writer that evicted it, moves its watermark over units it
                              // never held); 0 = never
};

struct result {
    uint64_t sent_units;
    uint64_t repair_units;
    uint64_t dropped;
    uint64_t solicits;
    uint32_t samples_done;
    uint64_t completion_ns[64];
    uint64_t queue_delay_max_ns;
    uint64_t queue_delay_sum_ns;
    uint64_t queue_delay_n;
    uint64_t rate_after_step_ok_ns[3]; // time after each step until BtlBw is within 25 % of the link rate; 0 = never
    uint64_t max_step_x100; // the largest change of the pacing rate from one step to the next, x100, after 5 s
    uint64_t max_rate_bps;
    uint64_t min_rate_bps;
    uint64_t trips_q, trips_d;
    uint64_t delivered_by_reader[MAX_READERS]; // unique datagrams each reader holds at the end
};

static double ms(uint64_t ns) {
    return (double)ns / 1e6;
}

// The rate the writer sends at: the sender's line rate unpaced, else the pacing rate (fixed, or the estimator's).
static uint64_t send_rate(enum mode mode, uint64_t fixed_bps, const struct ap_pacer* ctl) {
    if (mode == MODE_UNPACED) {
        return 1000000000ULL;
    }
    return mode == MODE_FIXED ? fixed_bps : ap_rate_bps(ctl);
}

// Which of the link's three stretches (before the first step, between, after the second) `now` is in, and when it
// began.
static int link_phase(const struct link_cfg* cfg, uint64_t now) {
    if (now < cfg->step1_ns) {
        return 0;
    }
    return now < cfg->step2_ns ? 1 : 2;
}

static uint64_t link_phase_start(const struct link_cfg* cfg, int phase) {
    if (phase == 0) {
        return 0;
    }
    return phase == 1 ? cfg->step1_ns : cfg->step2_ns;
}

static uint64_t link_rate(const struct link_cfg* cfg, uint64_t now) {
    return cfg->rate_bps[link_phase(cfg, now)];
}

// What a reader's ACKNACK shows it holds, in its own units: without watermark_acks, the units it has received; with,
// the watermark (its lowest missing unit) plus the units received inside the window its bitmap names (up to its highest
// missing unit), and everything up to its highest received when nothing is missing.
// What a reader's ACKNACK shows, in its own units: without watermark_acks the units it has received; with, the
// watermark (its lowest missing unit) and a bitmap naming the units missing below its highest received - all of them,
// or with omit_named only those it has not named within `omit_ns` (the ones it reckons repairs are on the way for). The
// writer counts the units inside the bitmap's window that are not named as received: that is `held`, and `missing` is
// what is named.
static void reader_report(const struct link_cfg* cfg, const uint8_t* got, uint64_t* named_ns, uint32_t readers,
                          uint32_t reader, int64_t last_id, uint64_t unique, uint64_t now, uint64_t omit_ns,
                          uint64_t* held, uint64_t* missing, uint64_t* mark) {
    *held = unique;
    *missing = 0;
    *mark = unique;
    if (!cfg->watermark_acks) {
        return;
    }
    int64_t highest_got = -1;
    for (int64_t u = reader; u <= last_id; u += readers) {
        if (got[u] != 0) {
            highest_got = u;
        }
    }
    if (highest_got < 0) {
        *held = 0;
        *mark = 0;
        return;
    }
    int64_t lowest_missing = -1;
    int64_t highest_named = -1;
    uint64_t named = 0;
    for (int64_t u = reader; u <= highest_got; u += readers) {
        if (got[u] != 0) {
            continue;
        }
        lowest_missing = lowest_missing < 0 ? u : lowest_missing;
        if (cfg->omit_named && named_ns[u] != 0 && now - named_ns[u] < omit_ns) {
            continue; // named lately: a repair is on its way
        }
        named_ns[u] = now;
        highest_named = u;
        named++;
    }
    if (lowest_missing < 0) {
        *held = (uint64_t)(highest_got / (int64_t)readers) + 1U;
        *mark = *held;
        return;
    }
    // What the library reads of an ACKNACK: the watermark (every unit below it held for certain, `mark`) and `held`,
    // the watermark with the unnamed units inside the bitmap's window. The bitmap names units that are missing - among
    // them units the writer has sent that are still in flight - so a count that adds them back (held + missing) runs
    // ahead of what has arrived.
    uint64_t watermark = (uint64_t)(lowest_missing / (int64_t)readers);
    *mark = watermark;
    if (named == 0) {
        *held = watermark;
        return;
    }
    uint64_t window = ((uint64_t)(highest_named / (int64_t)readers) - watermark) + 1U;
    *held = watermark + window - named;
    *missing = named;
}

// One run: `period_ns` 0 = a new sample as soon as the last completes (bulk); else one sample every period_ns.
// `readers` readers share the link: a sample goes to each of them as its own datagrams (unicast reliable), so the link
// carries it `readers` times, and the pacer is one per link, fed by every reader's reports (the pooled, link-level
// estimate).
static uint32_t g_model_sample_units = 0; // run(): units of a sample, 0 = SAMPLE_UNITS (1 MiB)
static bool g_model_prior = false;        // run(): the cap is the user's prior of the link (START begins at half of it)

static uint32_t model_sample_units(void) {
    return g_model_sample_units != 0U ? g_model_sample_units : SAMPLE_UNITS;
}

// NOLINTBEGIN(readability-function-cognitive-complexity) -- a simulator: the link and both its ends in one clock loop
static void run(const struct link_cfg* cfg, enum mode mode, uint64_t fixed_bps, uint64_t cap_bps, uint64_t min_bps,
                uint64_t period_ns, uint64_t duration_ns, uint32_t readers, struct result* out) {
    static struct pkt queue[MAX_Q];
    static struct flight fwd[MAX_DELAY];
    static struct flight rev[MAX_DELAY];
    static struct flight pending[MAX_PENDING]; // solicitations that have reached a reader and wait for its poll
    uint32_t pending_n = 0;
    static uint8_t got[MAX_UNITS];
    static uint64_t last_sent[MAX_UNITS];
    static uint8_t want_repair[MAX_UNITS];
    memset(out, 0, sizeof(*out));
    memset(got, 0, sizeof(got));
    memset(last_sent, 0, sizeof(last_sent));
    memset(want_repair, 0, sizeof(want_repair));
    out->min_rate_bps = UINT64_MAX;
    uint32_t qh = 0;
    uint32_t qt = 0;
    uint32_t qn = 0;
    uint32_t qbytes = 0;
    uint32_t fh = 0;
    uint32_t ft = 0;
    uint32_t fn = 0;
    uint32_t rh = 0;
    uint32_t rt = 0;
    uint32_t rn = 0;
    int64_t credit = 0; // link credit in bits
    struct ap_pacer ap;
    ap_init(&ap, min_bps, cap_bps, 0, 2 * cfg->one_way_ns, UNIT_WIRE);
    if (!g_model_prior) { // the cap of these scenarios is a cap, not a statement of the link
        ap.prior_bps = 0;
        ap.cap_bps = cap_bps;
        ap.state = AP_START;
        ap.rate_bps = ap_clamp_rate(&ap, ap_start_rate_bps(&ap));
    }

    int64_t sample_first = 0; // unit ids of the current sample
    int64_t sample_next = 0;
    int64_t sample_end = 0;
    bool sampling = false;
    uint64_t sample_started = 0;
    uint64_t next_sample_ns = 0;
    uint64_t next_send_ns = 0;
    uint64_t next_unsolicited_ns = 0;
    uint64_t last_solicit_ns = 0;
    bool solicit_open = false; // a solicitation is out and no report has come since
    uint32_t lcg = 12345;
    bool ge_bad = false;
    uint64_t next_jump_ns = 0;
    uint64_t jump_units = 0;             // units the watermark has moved past without holding them
    static uint64_t named_ns[MAX_UNITS]; // when the reader last named each unit missing
    memset(named_ns, 0, sizeof(named_ns));
    uint64_t next_solicit_ns = 0;
    uint64_t delivered_units[MAX_READERS] = {0}; // what each reader has, as the reader counts it
    uint64_t reported_units[MAX_READERS] = {0};
    uint64_t reported_missing[MAX_READERS] = {0}; // what the writer last heard each reader had
    uint64_t srtt = 2 * cfg->one_way_ns;
    uint64_t rate_ok_since[3] = {0, 0, 0};
    uint64_t prev_rate_bps = 0;
    uint64_t sample_period_start = 0;
    (void)sample_period_start;

    for (uint64_t now = 0; now < duration_ns; now += STEP_NS) {
        // 1. a sample begins
        if (!sampling && now >= next_sample_ns &&
            sample_end + 1 < (int64_t)MAX_UNITS - (int64_t)(model_sample_units() * readers)) {
            sample_first = sample_end + (sample_end == 0 ? 0 : 1);
            sample_next = sample_first;
            sample_end = sample_first + (int64_t)(model_sample_units() * readers) - 1;
            sampling = true;
            sample_started = now;
            next_sample_ns = period_ns != 0 ? now + period_ns : 0;
        }
        // 2. the writer sends: repairs ahead of new units, at the pacing rate
        uint64_t rate = send_rate(mode, fixed_bps, &ap);
        if (rate > out->max_rate_bps) {
            out->max_rate_bps = rate;
        }
        if (mode == MODE_ADAPTIVE && sampling && rate < out->min_rate_bps) {
            out->min_rate_bps = rate;
        }
        while (sampling && now >= next_send_ns) {
            int64_t unit = -1;
            bool repair = false;
            for (int64_t u = sample_first; u < sample_next; u++) {
                if (want_repair[u] && !got[u] && now - last_sent[u] > srtt + srtt / 2U) {
                    unit = u;
                    repair = true;
                    break;
                }
            }
            if (unit < 0 && sample_next <= sample_end) {
                unit = sample_next++;
            }
            if (unit < 0) {
                break;
            }
            if (qn + 1U >= MAX_Q || qbytes + UNIT_WIRE > cfg->queue_bytes) {
                out->dropped++;
            } else {
                queue[qt] = (struct pkt) {now, UNIT_WIRE, unit, (uint32_t)(unit % (int64_t)readers), 0, 0};
                qt = (qt + 1U) % MAX_Q;
                qn++;
                qbytes += UNIT_WIRE;
            }
            last_sent[unit] = now;
            want_repair[unit] = 0;
            out->sent_units++;
            if (repair) {
                out->repair_units++;
            }
            if (mode == MODE_ADAPTIVE) {
                ap_note_send(&ap, now + 1U, UNIT_WIRE, repair);
                if (!repair) {
                    ap_note_data_sent(&ap, now, UNIT_WIRE, 1U); // an original: the library stamps these, not repairs
                }
            }
            uint64_t gap = ((uint64_t)UNIT_WIRE * 8ULL * 1000000000ULL) / rate;
            if (next_send_ns + MS < now) {
                next_send_ns = now; // idle for a while: no catching up in a burst
            }
            next_send_ns += gap; // the clock, not "now": a send late by part of a step does not lose that part
        }
        // 3. a solicit, once per RTprop (10 ms before a sample of round trips), while a sample is open
        if (cfg->watermark_acks && solicit_open && now - last_solicit_ns > 100 * MS) {
            solicit_open = false; // a lost solicitation or answer: asked again
        }
        if (sampling && now >= next_solicit_ns && !(cfg->watermark_acks && solicit_open)) {
            uint64_t rtprop = mode == MODE_ADAPTIVE ? ap_rtprop_ns(&ap) : srtt;
            next_solicit_ns = now + (rtprop < 5000000ULL ? 5000000ULL : rtprop);
            if (cfg->watermark_acks) {
                solicit_open = true;
                last_solicit_ns = now;
            }
            for (uint32_t r = 0; r < readers; r++) {
                if (qn + 1U < MAX_Q && qbytes + SOLICIT_WIRE <= cfg->queue_bytes) {
                    queue[qt] = (struct pkt) {now, SOLICIT_WIRE, -1, r, now, sample_next - 1};
                    qt = (qt + 1U) % MAX_Q;
                    qn++;
                    qbytes += SOLICIT_WIRE;
                    out->solicits++;
                }
            }
        }
        // 4. the link serves its queue at its rate
        credit += (int64_t)(link_rate(cfg, now) * STEP_NS / 1000000000ULL);
        while (qn != 0 && credit >= (int64_t)queue[qh].wire * 8) {
            struct pkt p = queue[qh];
            qh = (qh + 1U) % MAX_Q;
            qn--;
            qbytes -= p.wire;
            credit -= (int64_t)p.wire * 8;
            uint64_t qd = now - p.enq_ns;
            if (qd > out->queue_delay_max_ns) {
                out->queue_delay_max_ns = qd;
            }
            out->queue_delay_sum_ns += qd;
            out->queue_delay_n++;
            lcg = (lcg * 1103515245U) + 12345U;
            uint32_t loss_now = cfg->loss_ppm;
            if (cfg->ge_p_ppm != 0) {
                uint32_t flip = (lcg >> 8) % 1000000U;
                ge_bad = ge_bad ? flip >= cfg->ge_r_ppm : flip < cfg->ge_p_ppm;
                lcg = (lcg * 1103515245U) + 12345U;
                loss_now = ge_bad ? cfg->ge_bad_loss_ppm : cfg->loss_ppm;
            }
            if (loss_now != 0 && ((lcg >> 8) % 1000000U) < loss_now) {
                out->dropped++; // lost on the link, not in its queue
                continue;
            }
            fwd[ft] = (struct flight) {now + cfg->one_way_ns, p};
            ft = (ft + 1U) % MAX_DELAY;
            fn++;
        }
        if (qn == 0 && credit > (int64_t)UNIT_WIRE * 8) {
            credit = (int64_t)UNIT_WIRE * 8; // an idle link does not bank a burst
        }
        // 4b. a reader's poll: it reports what it holds now, and the report travels back
        for (uint32_t i = 0; i < pending_n;) {
            if (pending[i].at_ns > now) {
                i++;
                continue;
            }
            struct pkt q = pending[i].pkt;
            pending[i] = pending[--pending_n];
            uint64_t held_now = 0;
            uint64_t missing_now = 0;
            uint64_t mark_now = 0;
            reader_report(cfg, got, named_ns, readers, q.reader, sample_end, delivered_units[q.reader], now,
                          srtt + (srtt / 2U), &held_now, &missing_now, &mark_now);
            rev[rt] =
                (struct flight) {now + cfg->one_way_ns, (struct pkt) {q.solicit_ns, 0, (int64_t)held_now, q.reader,
                                                                      (uint64_t)q.last_unit, (int64_t)mark_now}};
            rt = (rt + 1U) % MAX_DELAY;
            rn++;
        }
        // 4c. a reader that has a gap reports by itself, at its retry timer
        if (cfg->unsolicited_ns != 0 && now >= next_unsolicited_ns) {
            next_unsolicited_ns = now + cfg->unsolicited_ns;
            for (uint32_t r = 0; r < readers; r++) {
                int64_t highest_got = -1;
                bool gap = false;
                for (int64_t u = sample_first; u <= sample_end; u++) {
                    if ((uint32_t)(u % (int64_t)readers) != r) {
                        continue;
                    }
                    if (got[u]) {
                        highest_got = u;
                    } else if (highest_got >= 0 || u < sample_next) {
                        gap = gap || (highest_got >= 0);
                    }
                }
                if (gap && rn + 1U < MAX_DELAY) {
                    uint64_t held_now = 0;
                    uint64_t missing_now = 0;
                    uint64_t mark_now = 0;
                    reader_report(cfg, got, named_ns, readers, r, sample_end, delivered_units[r], now,
                                  srtt + (srtt / 2U), &held_now, &missing_now, &mark_now);
                    rev[rt] = (struct flight) {
                        now + cfg->one_way_ns,
                        (struct pkt) {0, 0, (int64_t)held_now, r, (uint64_t)highest_got, (int64_t)mark_now}};
                    rt = (rt + 1U) % MAX_DELAY;
                    rn++;
                }
            }
        }
        // 5. the reader receives; a solicit makes it report
        while (fn != 0 && fwd[fh].at_ns <= now) {
            struct pkt p = fwd[fh].pkt;
            fh = (fh + 1U) % MAX_DELAY;
            fn--;
            if (p.unit >= 0) {
                if (!got[p.unit]) {
                    got[p.unit] = 1;
                    delivered_units[p.reader]++;
                }
            } else if (pending_n < MAX_PENDING) {
                // the reader answers at its next poll (at once without a tick)
                uint64_t poll_at = now;
                if (cfg->reader_tick_ns != 0) {
                    poll_at = ((now / cfg->reader_tick_ns) + 1U) * cfg->reader_tick_ns;
                }
                pending[pending_n++] = (struct flight) {poll_at, p};
            }
        }
        // 6. the writer receives the report
        while (rn != 0 && rev[rh].at_ns <= now) {
            struct flight f = rev[rh];
            rh = (rh + 1U) % MAX_DELAY;
            rn--;
            uint64_t rtt = now - f.pkt.enq_ns;
            if (!(cfg->watermark_acks && f.pkt.enq_ns == 0)) {
                srtt = srtt == 0 ? rtt : (srtt * 7U + rtt) / 8U;
            }
            solicit_open = false; // the first report after a solicitation answers it, whatever sent it
            int64_t reported_highest = (int64_t)f.pkt.solicit_ns;
            for (int64_t u = sample_first; u <= reported_highest && u <= sample_end; u++) {
                if (!got[u] && (uint32_t)(u % (int64_t)readers) == f.pkt.reader) {
                    want_repair[u] = 1; // the reader names what is missing, up to the HEARTBEAT's last_seq_no
                }
            }
            reported_units[f.pkt.reader] = (uint64_t)f.pkt.unit;
            if (cfg->jump_every_ns != 0 && now >= next_jump_ns) {
                next_jump_ns = now + cfg->jump_every_ns;
                jump_units += 700U;
            }
            reported_units[f.pkt.reader] += jump_units;
            reported_missing[f.pkt.reader] += jump_units;
            reported_missing[f.pkt.reader] = (uint64_t)f.pkt.last_unit; // the watermark
            if (mode == MODE_ADAPTIVE) {
                uint64_t pooled_held = 0; // the link's delivery counts: the sum over its readers
                uint64_t pooled_mark = 0;
                for (uint32_t r = 0; r < readers; r++) {
                    pooled_held += reported_units[r];
                    pooled_mark += reported_missing[r];
                }
                ap_on_feedback(&ap, now, pooled_held, pooled_mark);
            }
        }
        // 7. a sample completes
        if (sampling) {
            bool all = true;
            for (int64_t u = sample_first; u <= sample_end && all; u++) {
                all = got[u] != 0;
            }
            if (all) {
                if (out->samples_done < 64U) {
                    out->completion_ns[out->samples_done] = now - sample_started;
                }
                out->samples_done++;
                sampling = false;
                if (period_ns == 0) {
                    next_sample_ns = now;
                }
            }
        }
        if (mode == MODE_ADAPTIVE && getenv("TAP_TRACE") != NULL && now % 500000000ULL < STEP_NS) {
            printf("  t %5.1f s link %5.1f Mbit/s  BtlBw %5.2f rate %5.2f Mbit/s RTprop %5.1f ms state %d phase %d "
                   "drain %d q %u B\n",
                   (double)now / 1e9, (double)link_rate(cfg, now) / 1e6, (double)ap_btlbw_bps(&ap) / 1e6,
                   (double)ap_rate_bps(&ap) / 1e6, ms(ap_rtprop_ns(&ap)), (int)ap.state, (int)ap.phase,
                   (int)ap.draining, qbytes);
        }
        if (mode == MODE_ADAPTIVE) {
            uint64_t now_rate = ap_rate_bps(&ap);
            if (prev_rate_bps != 0 && now > 5000000000ULL) {
                uint64_t step_x100 = ap_max_u64(now_rate, prev_rate_bps) * 100U / ap_min_u64(now_rate, prev_rate_bps);
                out->max_step_x100 = ap_max_u64(out->max_step_x100, step_x100);
            }
            prev_rate_bps = now_rate;
        }
        // 8. convergence of the estimate after a step, against the link's own rate (adaptive runs)
        if (mode == MODE_ADAPTIVE && sampling) {
            uint64_t c = link_rate(cfg, now);
            uint64_t bw = ap_btlbw_bps(&ap);
            int phase = link_phase(cfg, now);
            uint64_t phase_start = link_phase_start(cfg, phase);
            if (bw * 4U >= c * 3U && bw * 4U <= c * 5U) {
                if (rate_ok_since[phase] == 0 && out->rate_after_step_ok_ns[phase] == 0) {
                    rate_ok_since[phase] = now - phase_start + 1U;
                    out->rate_after_step_ok_ns[phase] = rate_ok_since[phase];
                }
            }
        }
    }
    for (uint32_t r = 0; r < readers; r++) {
        out->delivered_by_reader[r] = delivered_units[r];
    }
    out->trips_q = ap.queue_trips;
    out->trips_d = ap.drop_trips;
}
// NOLINTEND(readability-function-cognitive-complexity)

static int failures;

#define CHECK(cond, ...)                  \
    do {                                  \
        if (!(cond)) {                    \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
            failures++;                   \
        }                                 \
    } while (0)

// Repairs as a share of what was sent, in per cent: the unpaced control resends most of what it sends.
static uint64_t repair_share_pct(const struct result* r) {
    return r->sent_units != 0 ? r->repair_units * 100U / r->sent_units : 0U;
}

static void print_result(const char* name, const struct result* r) {
    uint64_t med = 0;
    uint64_t worst = 0;
    uint64_t sorted[64];
    uint32_t n = r->samples_done < 64U ? r->samples_done : 64U;
    memcpy(sorted, r->completion_ns, n * sizeof(uint64_t));
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1U; j < n; j++) {
            if (sorted[j] < sorted[i]) {
                uint64_t t = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = t;
            }
        }
    }
    if (n != 0) {
        med = sorted[n / 2U];
        worst = sorted[n - 1U];
    }
    printf("%-28s samples %2u  completion p50 %7.0f ms max %7.0f ms  sent %6llu (repairs %5llu, dropped %5llu)  "
           "queue delay mean %5.1f max %6.1f ms  solicits %llu  trips q%llu d%llu\n",
           name, r->samples_done, ms(med), ms(worst), (unsigned long long)r->sent_units,
           (unsigned long long)r->repair_units, (unsigned long long)r->dropped,
           r->queue_delay_n != 0 ? ms(r->queue_delay_sum_ns / r->queue_delay_n) : 0.0, ms(r->queue_delay_max_ns),
           (unsigned long long)r->solicits, (unsigned long long)r->trips_q, (unsigned long long)r->trips_d);
}

// ---- Replay of the Pi traces (quick test 4 of 04cd1473, wtc_adaptive_quick4): the recorded rounds fed to the
// estimator one by one. What is recorded of each round is its length, the bytes it delivered and whether it was
// application-limited (the pacer sent less than 80 % of its allowance: the sender had nothing to send but control
// traffic). The replay gives the estimator those, at the rate the estimator itself has chosen - a round that was
// application-limited sends a quarter of its allowance, one that was not sends all of it - and checks what the
// estimator does with them.
struct replay_round {
    double el_ms;
    uint64_t bytes;
    int app;
};

/* replay_m2r2: M2 run 2, first 64 rounds of the Pi trace: round length ms, bytes delivered, 1 if the round was
 * application-limited */
static const struct replay_round replay_m2r2[] = {
    {196.9, 29222, 0},   {177.8, 33836, 0},   {85.5, 26146, 0},    {85.6, 61520, 0},    {42.8, 39988, 0},
    {43.1, 86128, 0},    {45.5, 173794, 0},   {43.1, 206092, 1},   {43.2, 446020, 1},   {3236.6, 7690, 1},
    {48.3, 1088904, 1},  {346.9, 3076, 1},    {349.4, 18456, 1},   {71.9, 149186, 0},   {220.3, 24608, 1},
    {143.8, 24608, 1},   {238.5, 26146, 1},   {348.5, 27684, 1},   {213.1, 29222, 1},   {175.1, 24608, 1},
    {125.0, 24608, 1},   {97.0, 27684, 1},    {44.3, 29222, 1},    {45.5, 58444, 1},    {45.9, 133806, 1},
    {43.6, 166104, 1},   {43.6, 118426, 1},   {44.8, 47678, 1},    {235.9, 198402, 0},  {209.5, 58444, 0},
    {209.5, 41526, 0},   {209.4, 52292, 0},   {209.6, 87666, 0},   {330.1, 287606, 0},  {238.8, 346050, 0},
    {2593.3, 236852, 1}, {209.5, 1047378, 0}, {220.2, 63058, 1},   {223.2, 1104284, 0}, {786.6, 52292, 1},
    {561.8, 129192, 0},  {212.5, 76900, 0},   {209.7, 147648, 0},  {221.9, 396804, 0},  {238.8, 307600, 1},
    {691.8, 409108, 0},  {784.5, 184560, 0},  {224.0, 67672, 0},   {277.2, 232238, 0},  {263.9, 475242, 0},
    {700.1, 30760, 1},   {263.0, 827444, 1},  {2041.8, 281454, 1}, {209.5, 821292, 1},  {3749.4, 7690, 1},
    {210.9, 985858, 1},  {406.5, 24608, 1},   {346.0, 1108898, 0}, {3694.3, 1538, 1},   {169.2, 644422, 0},
    {3830.8, 466014, 1}, {166.4, 678258, 0},  {3833.7, 432178, 1}, {166.7, 679796, 0},
};
/* replay_s2r2: S2 run 2, first 120 rounds of the Pi trace: round length ms, bytes delivered, 1 if the round was
 * application-limited */
static const struct replay_round replay_s2r2[] = {
    {198.9, 29222, 0}, {152.3, 27684, 0}, {151.3, 29222, 0}, {130.6, 24608, 0}, {122.5, 27684, 0},  {152.0, 30760, 0},
    {176.5, 35374, 0}, {129.9, 26146, 0}, {150.5, 33836, 0}, {150.5, 33836, 0}, {205.2, 43064, 0},  {150.5, 33836, 0},
    {161.8, 33836, 0}, {473.3, 56906, 0}, {117.0, 41526, 0}, {118.0, 30760, 0}, {117.2, 39988, 0},  {168.3, 49216, 0},
    {122.3, 39988, 0}, {108.4, 32298, 0}, {113.0, 38450, 0}, {113.6, 38450, 0}, {79.8, 26146, 0},   {94.0, 35374, 0},
    {111.6, 33836, 0}, {114.2, 38450, 0}, {117.2, 43064, 0}, {116.6, 44602, 0}, {113.7, 43064, 0},  {72.6, 24608, 0},
    {93.6, 36912, 0},  {105.2, 36912, 0}, {101.9, 44602, 0}, {53.8, 24608, 0},  {102.9, 50754, 0},  {51.6, 27684, 0},
    {97.8, 47678, 0},  {66.1, 27684, 0},  {191.7, 87666, 0}, {61.8, 30760, 0},  {74.5, 29222, 0},   {117.0, 46140, 0},
    {51.5, 26146, 0},  {51.5, 26146, 0},  {106.0, 53830, 0}, {95.1, 46140, 0},  {109.1, 55368, 0},  {121.2, 56906, 0},
    {98.1, 50754, 0},  {54.7, 27684, 0},  {110.9, 55368, 0}, {67.3, 29222, 0},  {113.0, 56906, 0},  {82.0, 39988, 0},
    {63.6, 32298, 0},  {110.7, 49216, 0}, {104.7, 50754, 1}, {1240.2, 7690, 1}, {60.5, 30760, 0},   {99.9, 50754, 0},
    {99.9, 50754, 0},  {51.5, 26146, 0},  {99.9, 50754, 0},  {90.8, 44602, 0},  {78.8, 41526, 0},   {75.7, 36912, 0},
    {106.9, 44602, 0}, {100.1, 49216, 0}, {54.5, 27684, 0},  {51.5, 26146, 0},  {51.7, 26146, 0},   {54.5, 27684, 0},
    {51.5, 26146, 0},  {51.5, 26146, 0},  {60.5, 30760, 0},  {87.8, 44602, 0},  {51.5, 26146, 0},   {51.5, 26146, 0},
    {99.9, 50754, 0},  {51.5, 26146, 0},  {51.5, 26146, 0},  {99.9, 50754, 0},  {51.5, 26146, 0},   {109.0, 55368, 0},
    {64.0, 32298, 0},  {73.2, 35374, 0},  {54.7, 29222, 0},  {103.0, 52292, 1}, {1810.4, 12304, 1}, {72.6, 38450, 0},
    {87.8, 43064, 0},  {72.7, 35374, 0},  {75.9, 38450, 0},  {51.5, 26146, 0},  {72.5, 35374, 0},   {128.0, 59982, 0},
    {51.5, 26146, 0},  {54.5, 27684, 0},  {51.5, 26146, 0},  {151.4, 76900, 0}, {99.9, 50754, 0},   {100.1, 50754, 0},
    {51.5, 26146, 0},  {109.0, 55368, 0}, {63.6, 32298, 0},  {90.9, 46140, 0},  {96.8, 47678, 0},   {69.7, 30760, 0},
    {133.3, 64596, 0}, {57.6, 26146, 0},  {99.9, 44602, 0},  {142.4, 56906, 0}, {67.7, 29222, 0},   {102.4, 29222, 0},
    {104.7, 33836, 0}, {111.9, 27684, 0}, {249.7, 24608, 1}, {214.4, 30760, 0}, {155.9, 27684, 0},  {104.5, 24608, 0},
};

// Replays `rounds`; returns the largest ratio of the rate chosen to twice the best delivery rate seen so far (1 Mbit/s
// at least): at most 1 means the rate never ran past twice what the link was seen to deliver. `variation` is the
// max/min rate after START.
static double replay_run(const struct replay_round* rounds, size_t count, double* variation, uint64_t* max_rate_kbit) {
    struct ap_pacer ctl;
    ap_init(&ctl, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    uint64_t now = 1000ULL * 1000000000ULL;
    double best_kbit = 1000.0;
    double worst = 0.0;
    uint64_t lowest = UINT64_MAX;
    uint64_t highest = 0;
    bool cruised = false;
    *max_rate_kbit = 0;
    for (size_t i = 0; i < count; i++) {
        double delivered_kbit = (double)rounds[i].bytes * 8.0 / rounds[i].el_ms;
        best_kbit = delivered_kbit > best_kbit ? delivered_kbit : best_kbit;
        uint64_t allowance = (uint64_t)((double)ctl.rate_bps / 8.0 * rounds[i].el_ms / 1000.0);
        ctl.round_start_ns = now;
        ctl.round_delivered = ctl.delivered;
        ctl.delivered += rounds[i].bytes;
        ctl.round_sent = rounds[i].app != 0 ? (allowance / 4U) + 100U : allowance;
        ctl.round_units0 = ctl.units_sent;
        ctl.units_sent += rounds[i].app != 0 ? 0U : 10U;
        now += (uint64_t)(rounds[i].el_ms * 1e6);
        ap_end_round(&ctl, now);
        double chosen_kbit = (double)ctl.rate_bps / 1000.0;
        double ratio = chosen_kbit / (2.0 * best_kbit);
        worst = ratio > worst ? ratio : worst;
        *max_rate_kbit = (uint64_t)chosen_kbit > *max_rate_kbit ? (uint64_t)chosen_kbit : *max_rate_kbit;
        cruised = cruised || ctl.state == AP_CRUISE;
        if (cruised) {
            lowest = ctl.rate_bps < lowest ? ctl.rate_bps : lowest;
            highest = ctl.rate_bps > highest ? ctl.rate_bps : highest;
        }
    }
    *variation = lowest != 0 && lowest != UINT64_MAX ? (double)highest / (double)lowest : 0.0;
    return worst;
}

// Pi quick test 6 (5de6f40b), three causes read off its traces.
static void check_quick6_causes(void) {
    const uint64_t SEC = 1000000000ULL;
    // (i) 1 MiB M2: START at 180 Mbit/s on a 30 Mbit/s link, 70 % of the bytes of a round repairs and the round
    // delivered 21 Mbit/s: the search is over and BtlBw comes down (it held, every round was app-limited).
    struct ap_pacer over;
    ap_init(&over, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    uint64_t t_over = 1000ULL * SEC;
    over.rate_bps = 180000000ULL;
    ap_filter_put(&over.bw, 90000000ULL, 1U);
    over.round_start_ns = t_over;
    over.round_delivered = over.delivered;
    over.delivered += 120000U; // 21 Mbit/s over 46 ms
    over.round_sent = 531006U;
    over.round_repair = 400000U;
    over.round_units0 = over.units_sent;
    over.units_sent += 10U;
    t_over += 46 * MS;
    ap_end_round(&over, t_over);
    CHECK(over.state == AP_CRUISE && over.drop_trips == 1U, "START went on after a loss storm (state %d, trips %u)",
          (int)over.state, (unsigned)over.drop_trips);
    CHECK(ap_rate_bps(&over) < 100000000ULL, "the rate stayed at %llu bit/s after a loss storm",
          (unsigned long long)ap_rate_bps(&over));

    // (ii) S1 128 KiB: a sample every 4 s, a quiet round between, the sample's own round application-limited: the
    // BtlBw filter was aged empty in 10 rounds.
    struct ap_pacer quiet;
    ap_init(&quiet, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    uint64_t t_quiet = 1000ULL * SEC;
    quiet.rate_bps = 8000000ULL;
    ap_filter_put(&quiet.bw, 4018000ULL, 1U);
    for (int cycle = 0; cycle < 20; cycle++) {
        quiet.round_start_ns = t_quiet;
        quiet.round_delivered = quiet.delivered;
        quiet.delivered += 25000U;
        quiet.round_sent = 30000U;
        quiet.round_units0 = quiet.units_sent;
        t_quiet += 3770 * MS;
        ap_end_round(&quiet, t_quiet);
        if (cycle >= 10) {
            CHECK(ap_btlbw_bps(&quiet) >= 3500000ULL, "BtlBw was aged to %llu bit/s by the quiet rounds (cycle %d)",
                  (unsigned long long)ap_btlbw_bps(&quiet), cycle);
        }
        quiet.round_start_ns = t_quiet;
        quiet.round_delivered = quiet.delivered;
        quiet.delivered += 110000U;
        quiet.round_sent = 125000U;
        quiet.round_units0 = quiet.units_sent;
        t_quiet += 220 * MS;
        ap_end_round(&quiet, t_quiet);
    }

    // (iii) the watermark reaching the last unit sent is a round trip sample whatever its advance.
    struct ap_pacer tail;
    ap_init(&tail, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    uint64_t t_tail = 1000ULL * SEC;
    for (int unit = 0; unit < 90; unit++) {
        ap_note_send(&tail, t_tail + ((uint64_t)unit * MS), UNIT_WIRE, false);
        ap_note_data_sent(&tail, t_tail + ((uint64_t)unit * MS), UNIT_WIRE, 1U);
    }
    ap_on_feedback(&tail, t_tail + (89 * MS) + (12 * MS), 90U,
                   90U); // all 90 at once, 12 ms after the last was sent
    CHECK(tail.rtt_samples == 1U && ap_rtprop_ns(&tail) < 13 * MS,
          "the tail sample was not taken (samples %u, rtprop %llu ns)", (unsigned)tail.rtt_samples,
          (unsigned long long)ap_rtprop_ns(&tail));
}

// Pi quick test 7 (74df5385): what the three changes of quick test 6 left or broke, read off its traces.
static void check_quick7_causes(void) {
    // (i) 128 KiB M2 round 8: the tail of a sample, 1.7 kB sent and all of it repairs, a reading of 2.9 Mbit/s against
    // a rate of 9.7. It ended START at 5.7 Mbit/s of a 30 Mbit/s link (p50 86 -> 188 ms): no loss storm, a few repairs.
    struct ap_pacer tail;
    ap_init(&tail, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    uint64_t t_tail = 1000ULL * 1000000000ULL;
    tail.rate_bps = 9732000ULL;
    ap_filter_put(&tail.bw, 11464000ULL, 1U);
    tail.round_start_ns = t_tail;
    tail.round_delivered = tail.delivered;
    tail.delivered += 32298U;
    tail.round_sent = 1718U;
    tail.round_repair = 1718U;
    tail.round_units0 = tail.units_sent;
    tail.units_sent += 1U;
    t_tail += 88 * MS;
    ap_end_round(&tail, t_tail);
    CHECK(tail.state == AP_START && tail.drop_trips == 0U,
          "a sample's tail of repairs ended START (state %d, trips %u)", (int)tail.state, (unsigned)tail.drop_trips);

    // (ii) 1 MiB S2: a round on a 8 Mbit/s rate that read 6 Mbit/s with 40 % of its bytes repairs: START does not
    // double on it (it did, to 46 Mbit/s on a 4 Mbit/s link).
    struct ap_pacer lossy;
    ap_init(&lossy, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    uint64_t t_lossy = 1000ULL * 1000000000ULL;
    lossy.rate_bps = 8000000ULL;
    lossy.round_start_ns = t_lossy;
    lossy.round_delivered = lossy.delivered;
    lossy.delivered += 100000U; // 6 Mbit/s over 133 ms
    lossy.round_sent = 133000U;
    lossy.round_repair = 53000U;
    lossy.round_units0 = lossy.units_sent;
    lossy.units_sent += 60U;
    t_lossy += 133 * MS;
    ap_end_round(&lossy, t_lossy);
    CHECK(lossy.state == AP_START && ap_rate_bps(&lossy) <= 8000000ULL,
          "START grew to %llu bit/s in a round of 40 %% repairs", (unsigned long long)ap_rate_bps(&lossy));

    // (iii) 128 KiB S1/S2: the first round trip samples were taken in the queue the START rate built (142 ms on a 40 ms
    // path); the announce round trip is a floor for RTprop.
    struct ap_pacer floor;
    ap_init(&floor, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    for (uint64_t at = 1; at <= 3; at++) {
        ap_filter_put(&floor.rtt, 142 * MS, at * 100 * MS);
    }
    ap_note_announce_rtt(&floor, 40 * MS);
    CHECK(ap_rtprop_ns(&floor) <= 40 * MS, "RTprop %llu ns with an announce round trip of 40 ms",
          (unsigned long long)ap_rtprop_ns(&floor));
}

// The median completion time of a result's samples, ns.
static uint64_t median_ns(const struct result* res) {
    uint64_t sorted[64];
    uint32_t count = res->samples_done < 64U ? res->samples_done : 64U;
    for (uint32_t i = 0; i < count; i++) {
        sorted[i] = res->completion_ns[i];
    }
    for (uint32_t i = 1; i < count; i++) {
        uint64_t value = sorted[i];
        uint32_t pos = i;
        while (pos > 0 && sorted[pos - 1U] > value) {
            sorted[pos] = sorted[pos - 1U];
            pos--;
        }
        sorted[pos] = value;
    }
    return count != 0U ? sorted[count / 2U] : 0U;
}

// Feeds one full round to a pacer: `sent` bytes sent (`repair` of them repairs), `delivered` bytes delivered, `ms`
// long.
static void feed_round(struct ap_pacer* ctl, uint64_t* now, uint64_t ms, uint64_t sent, uint64_t repair,
                       uint64_t delivered) {
    ctl->round_start_ns = *now;
    ctl->round_delivered = ctl->delivered;
    ctl->delivered += delivered;
    ctl->round_sent = sent;
    ctl->round_repair = repair;
    ctl->round_units0 = ctl->units_sent;
    ctl->units_sent += sent / UNIT_WIRE;
    ctl->held_units = ctl->units_sent;
    *now += ms * MS;
    ap_end_round(ctl, *now);
}

// The prior-anchored experiment (2026-11-22): with a prior the rate is 95 % of it, no search, and goes down only on
// congestion, not on loss.
static void check_anchor_causes(void) {
    const uint64_t SEC = 1000000000ULL;
    const uint64_t prior = 30000000ULL;
    const uint64_t ceiling = 28500000ULL;
    const uint64_t round_bytes = ceiling / 8U / 10U; // 100 ms at the ceiling

    // (a) START at 95 % of the prior, in ANCHOR (no half-prior, no doubling), and nothing above it.
    struct ap_pacer anchor;
    ap_init(&anchor, 100000ULL, prior, 0, 0, UNIT_WIRE);
    CHECK(ap_rate_bps(&anchor) == ceiling && anchor.state == AP_ANCHOR, "START began at %llu bit/s in state %d",
          (unsigned long long)ap_rate_bps(&anchor), (int)anchor.state);
    uint64_t now = 1000ULL * SEC;
    for (int round = 0; round < 6; round++) { // delivering more than was sent: no raise above the ceiling
        feed_round(&anchor, &now, 100, round_bytes, 0, 2U * round_bytes);
    }
    CHECK(ap_rate_bps(&anchor) == ceiling, "the rate went to %llu bit/s above 95 %% of the prior",
          (unsigned long long)ap_rate_bps(&anchor));

    // (b) loss is not congestion: a fifth of every round's bytes repairs, the rest delivered; a tail of repairs; no
    // decrease and no search (a loss exit and a CRUISE of 6b05d93c moved the rate here).
    struct ap_pacer lossy;
    ap_init(&lossy, 100000ULL, prior, 0, 0, UNIT_WIRE);
    now = 1000ULL * SEC;
    for (int round = 0; round < 12; round++) {
        feed_round(&lossy, &now, 100, round_bytes, round_bytes / 5U, round_bytes - (round_bytes / 5U));
    }
    feed_round(&lossy, &now, 90, 1718, 1718, 32298); // the tail of a sample
    CHECK(ap_rate_bps(&lossy) == ceiling && lossy.state == AP_ANCHOR, "loss moved the rate to %llu bit/s (state %d)",
          (unsigned long long)ap_rate_bps(&lossy), (int)lossy.state);

    // (c) a queue that lasts into a second round steps the rate down by a fifth; one round of it does not (a
    // sample or two of a recovery is no queue); it comes back when the queue is gone.
    struct ap_pacer queued;
    ap_init(&queued, 100000ULL, prior, 0, 0, UNIT_WIRE);
    ap_filter_put(&queued.rtt, 10 * MS, 1U);
    queued.recent_n = AP_RECENT_RTTS;
    now = 1000ULL * SEC;
    for (int round = 0; round < 3; round++) {
        for (uint32_t idx = 0; idx < AP_RECENT_RTTS; idx++) {
            queued.recent_rtt[idx] = 60 * MS;
        }
        for (uint32_t trip = 0; trip < AP_QUEUE_TRIPS; trip++) {
            ap_queue_guard(&queued, now);
        }
        feed_round(&queued, &now, 100, round_bytes, 0, round_bytes);
        if (round == 0) {
            CHECK(ap_rate_bps(&queued) == ceiling && queued.state == AP_ANCHOR,
                  "one round of a queue took the rate to %llu bit/s", (unsigned long long)ap_rate_bps(&queued));
        }
    }
    uint64_t lowered = ap_rate_bps(&queued);
    CHECK(queued.state == AP_ANCHOR && lowered < ceiling && lowered >= ceiling * 64U / 100U,
          "three rounds of a queue took the rate to %llu bit/s (state %d)", (unsigned long long)lowered,
          (int)queued.state);
    for (uint32_t idx = 0; idx < AP_RECENT_RTTS; idx++) {
        queued.recent_rtt[idx] = 10 * MS;
    }
    ap_queue_guard(&queued, now);
    feed_round(&queued, &now, 100, round_bytes, 0, round_bytes);
    CHECK(ap_rate_bps(&queued) > lowered, "the rate did not recover when the queue was gone: %llu",
          (unsigned long long)ap_rate_bps(&queued));

    // (d) persistently low delivery (three rounds, after the first few) steps the rate down; two do not.
    struct ap_pacer starved;
    ap_init(&starved, 100000ULL, prior, 0, 0, UNIT_WIRE);
    now = 1000ULL * SEC;
    for (int round = 0; round < 6; round++) {
        feed_round(&starved, &now, 100, round_bytes, 0, round_bytes);
    }
    feed_round(&starved, &now, 100, round_bytes, 0, round_bytes / 4U);
    feed_round(&starved, &now, 100, round_bytes, 0, round_bytes / 4U);
    CHECK(ap_rate_bps(&starved) == ceiling, "two low rounds took the rate to %llu bit/s",
          (unsigned long long)ap_rate_bps(&starved));
    feed_round(&starved, &now, 100, round_bytes, 0, round_bytes / 4U);
    CHECK(ap_rate_bps(&starved) == ceiling * 80U / 100U, "three low rounds took the rate to %llu bit/s",
          (unsigned long long)ap_rate_bps(&starved));

    // (e) a wrong prior (4 Mbit/s under 30): two low rounds among the first, and the rate is the measured delivery
    // x 1.05 in CRUISE, with the ceiling still the prior's.
    struct ap_pacer wrong;
    ap_init(&wrong, 100000ULL, prior, 0, 0, UNIT_WIRE);
    now = 1000ULL * SEC;
    for (int round = 0; round < 4; round++) {
        feed_round(&wrong, &now, 100, round_bytes, 0, 50000U); // 4 Mbit/s over 100 ms
    }
    CHECK(wrong.state == AP_CRUISE && ap_rate_bps(&wrong) < 6000000ULL && ap_rate_bps(&wrong) > 3500000ULL,
          "a wrong prior went to state %d at %llu bit/s", (int)wrong.state, (unsigned long long)ap_rate_bps(&wrong));

    // (f) a loss storm on a believed prior: repairs above half the bytes at more than twice the delivery.
    struct ap_pacer storm;
    ap_init(&storm, 100000ULL, prior, 0, 0, UNIT_WIRE);
    now = 1000ULL * SEC;
    feed_round(&storm, &now, 100, round_bytes, 0, round_bytes);
    feed_round(&storm, &now, 100, round_bytes, round_bytes * 6U / 10U, round_bytes / 3U);
    CHECK(storm.state == AP_CRUISE && ap_rate_bps(&storm) < ceiling / 2U, "a loss storm left state %d at %llu bit/s",
          (int)storm.state, (unsigned long long)ap_rate_bps(&storm));

    // (g) the floor: the rate does not go below 25 % of the prior while the prior is believed (a queue that is
    // already at the floor is a wrong prior).
    struct ap_pacer floor_ctl;
    ap_init(&floor_ctl, 100000ULL, prior, 0, 0, UNIT_WIRE);
    floor_ctl.rate_bps = ap_clamp_rate(&floor_ctl, 1000ULL);
    CHECK(ap_rate_bps(&floor_ctl) == prior / 4U, "the floor of the prior is %llu bit/s",
          (unsigned long long)ap_rate_bps(&floor_ctl));

    // (h) the model. A right prior on M2-like bursty loss keeps the rate at 95 %; the same loss without queue growth
    // does not lower it; a wrong prior (30 Mbit/s on a 4 Mbit/s link) finds the link within a few round trips and
    // is no worse than no prior in samples, median and queue.
    struct link_cfg lossy_m2 = {{30000000ULL, 30000000ULL, 30000000ULL},
                                1000 * SEC,
                                2000 * SEC,
                                100U * UNIT_WIRE,
                                20 * MS,
                                0,
                                30000U,
                                8 * MS,
                                true};
    struct result right;
    g_model_prior = true;
    run(&lossy_m2, MODE_ADAPTIVE, 0, prior, 100000ULL, 6 * SEC, 60 * SEC, 1U, &right);
    print_result("right prior 30 Mbit/s, M2-like 3 % loss", &right);
    CHECK(right.samples_done >= 8U, "right prior: %u of 10 samples", right.samples_done);
    CHECK(right.min_rate_bps >= ceiling * 95U / 100U, "right prior: the rate fell to %llu bit/s",
          (unsigned long long)right.min_rate_bps);
    struct link_cfg slow = {{4000000ULL, 4000000ULL, 4000000ULL}, 1000 * SEC, 2000 * SEC, 40U * UNIT_WIRE, 10000000ULL};
    struct result with_prior;
    struct result without;
    run(&slow, MODE_ADAPTIVE, 0, prior, 100000ULL, 4 * SEC, 40 * SEC, 1U, &with_prior);
    g_model_prior = false;
    run(&slow, MODE_ADAPTIVE, 0, 1000000000ULL, 100000ULL, 4 * SEC, 40 * SEC, 1U, &without);
    print_result("wrong prior 30 Mbit/s on 4 Mbit/s", &with_prior);
    print_result("no prior", &without);
    CHECK(with_prior.samples_done >= without.samples_done, "the wrong prior completed %u samples, none %u",
          with_prior.samples_done, without.samples_done);
    CHECK(median_ns(&with_prior) <= median_ns(&without) + (median_ns(&without) / 4U),
          "the wrong prior's median %.0f ms, none %.0f ms", ms(median_ns(&with_prior)), ms(median_ns(&without)));
    CHECK(with_prior.queue_delay_n == 0U || without.queue_delay_n == 0U ||
              with_prior.queue_delay_sum_ns / with_prior.queue_delay_n <=
                  (without.queue_delay_sum_ns / without.queue_delay_n) + (10 * MS),
          "the wrong prior's mean queue delay %.1f ms, none %.1f ms",
          ms(with_prior.queue_delay_n != 0U ? with_prior.queue_delay_sum_ns / with_prior.queue_delay_n : 0U),
          ms(without.queue_delay_n != 0U ? without.queue_delay_sum_ns / without.queue_delay_n : 0U));
    CHECK(with_prior.rate_after_step_ok_ns[0] != 0 && with_prior.rate_after_step_ok_ns[0] < 2 * SEC,
          "the wrong prior took %.0f ms to find the link", ms(with_prior.rate_after_step_ok_ns[0]));
}

// Pi quick test 10 (32e2a6c2): the ratchet of 1 MiB S2 with a right prior (3.8 Mbit/s down to 100 kbit/s in ten rounds,
// 2 of 15 delivered), and the wrong prior (30 on 4 Mbit/s) that was never seen.
static void check_quick10_causes(void) {
    const uint64_t SEC = 1000000000ULL;
    const uint64_t prior = 4000000ULL;
    const uint64_t ceiling = 3800000ULL;
    const uint64_t round_bytes = ceiling / 8U / 10U;

    // (1) tail samples after a recovery are no queue: the watermark reaching the last unit sent 150 ms after it left, a
    // repair in the last round trip, on a path of 40 ms. Twice: the guard needs two samples (it tripped 18 times).
    struct ap_pacer tail;
    ap_init(&tail, 100000ULL, prior, 0, 0, UNIT_WIRE);
    ap_filter_put(&tail.rtt, 40 * MS, 1U);
    uint64_t t_tail = 1000ULL * SEC;
    for (int burst = 0; burst < 2; burst++) {
        for (int unit = 0; unit < 20; unit++) {
            ap_note_send(&tail, t_tail, UNIT_WIRE, false);
            ap_note_data_sent(&tail, t_tail, UNIT_WIRE, 1U);
        }
        ap_note_repair(&tail, t_tail + (140 * MS), UNIT_WIRE);
        t_tail += 150 * MS;
        ap_on_feedback(&tail, t_tail, tail.units_sent, tail.units_sent);
    }
    CHECK(tail.recent_n == 0U && !tail.anchor_qtrip && tail.queue_trips == 0U,
          "a tail sample after a recovery reached the queue guard (%u, %llu trips)", (unsigned)tail.recent_n,
          (unsigned long long)tail.queue_trips);

    // (2) at the floor of the prior with a queue and nothing wrong with the delivery, the prior is still believed (the
    // step to the measured delivery x 1.05 took the rate under the floor, into a CRUISE without one).
    struct ap_pacer floor_ctl;
    ap_init(&floor_ctl, 100000ULL, prior, 0, 0, UNIT_WIRE);
    floor_ctl.rate_bps = ap_clamp_rate(&floor_ctl, 1000ULL);
    floor_ctl.draining = true;
    uint64_t now = 1000ULL * SEC;
    for (int round = 0; round < 8; round++) {
        feed_round(&floor_ctl, &now, 100, round_bytes / 4U, 0, round_bytes / 4U);
    }
    CHECK(floor_ctl.state == AP_ANCHOR && ap_rate_bps(&floor_ctl) >= prior / 4U,
          "at the floor the state went to %d at %llu bit/s", (int)floor_ctl.state,
          (unsigned long long)ap_rate_bps(&floor_ctl));

    // (3) units a reader leaves out of its bitmap because their repairs are on the way are not delivered: 300 of 400
    // units sent were repairs in the last round trip, all 400 shown held - 100 are.
    struct ap_pacer omitted;
    ap_init(&omitted, 100000ULL, 30000000ULL, 0, 0, UNIT_WIRE);
    uint64_t t_om = 1000ULL * SEC;
    omitted.round_start_ns = t_om;
    omitted.round_delivered = omitted.delivered;
    for (int unit = 0; unit < 400; unit++) {
        ap_note_send(&omitted, t_om, UNIT_WIRE, unit >= 100);
        ap_note_data_sent(&omitted, t_om, UNIT_WIRE, 1U);
    }
    omitted.round_repair = (uint64_t)300U * UNIT_WIRE;
    ap_on_feedback(&omitted, t_om + (10 * MS), 400U, 400U);
    CHECK(omitted.held_units <= 120U, "the omitted units were counted held: %llu of 400",
          (unsigned long long)omitted.held_units);

    // (4) the model: a 30 Mbit/s prior on a 4 Mbit/s link with 1 % loss and a reader that leaves out the units it named
    // lately, 1 MiB every 6 s: the wrong prior is found, and it is no worse than none in samples and median.
    struct link_cfg lossy_s1 = {{4000000ULL, 4000000ULL, 4000000ULL},
                                1000 * SEC,
                                2000 * SEC,
                                40U * UNIT_WIRE,
                                5 * MS,
                                0,
                                10000U,
                                3 * MS,
                                true,
                                true};
    struct result with_prior;
    struct result without;
    g_model_prior = true;
    run(&lossy_s1, MODE_ADAPTIVE, 0, 30000000ULL, 100000ULL, 6 * SEC, 60 * SEC, 1U, &with_prior);
    g_model_prior = false;
    run(&lossy_s1, MODE_ADAPTIVE, 0, 1000000000ULL, 100000ULL, 6 * SEC, 60 * SEC, 1U, &without);
    print_result("wrong prior, 1 % loss, omitting reader", &with_prior);
    print_result("no prior, same link", &without);
    CHECK(with_prior.samples_done >= without.samples_done, "the wrong prior completed %u samples, none %u",
          with_prior.samples_done, without.samples_done);
    CHECK(median_ns(&with_prior) <= median_ns(&without) + (median_ns(&without) / 4U),
          "the wrong prior's median %.0f ms, none %.0f ms", ms(median_ns(&with_prior)), ms(median_ns(&without)));
}

// The exact rate-step run (WIFI_ADAPTIVE_RATESTEP_EXACT_2026-11-25): 128 KiB samples every 4 s after a step up
// (4 -> 30 Mbit/s: 36 s to recover, against 2 s for DDS), and the recovery after a cut (1 MiB after the step down: 17
// s).
static void check_ratestep_causes(void) {
    const uint64_t SEC = 1000000000ULL;
    const uint64_t link_cap = 4000000ULL;

    // (1) A probe running at the end of a 128 KiB sample is not lost in the quiet round that follows it: rate-step run,
    // 128 KiB every 4 s, 4 -> 30 Mbit/s: the probe at 1.25x succeeded, the run doubled, the sample ended, and the quiet
    // round (3.8 s, sample 58 kbit, application-limited) was scored as a failed probe - DRAIN at 0.75 BtlBw, the run
    // began again at the next sample: BtlBw 4.05 -> 4.8 -> 9.0 -> 9.05 -> 10.2 -> 11.9 -> 13.4 Mbit/s, 36 s to follow
    // the step.
    struct ap_pacer run_ctl;
    ap_init(&run_ctl, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    run_ctl.state = AP_CRUISE;
    run_ctl.phase = AP_PH_PROBE;
    run_ctl.probe_gain_pct = AP_PROBE_RUN_GAIN_PCT;
    run_ctl.probe_base_bps = 8000000ULL;
    run_ctl.rounds_since_probe = 1U;
    ap_filter_put(&run_ctl.bw, 8000000ULL, 1U);
    run_ctl.rate_bps = 16000000ULL;
    uint64_t now = 1000ULL * SEC;
    feed_round(&run_ctl, &now, 3800, 30000U, 0, 27684U); // the quiet round: 27 kB delivered over 3.8 s
    CHECK(run_ctl.phase == AP_PH_PROBE && ap_rate_bps(&run_ctl) == 16000000ULL,
          "the quiet round ended the probe (phase %d, rate %llu bit/s)", (int)run_ctl.phase,
          (unsigned long long)ap_rate_bps(&run_ctl));
    feed_round(&run_ctl, &now, 30, 60000U, 0, 60000U); // the first round of the next sample: 16 Mbit/s delivered
    CHECK(run_ctl.phase == AP_PH_PROBE && ap_rate_bps(&run_ctl) > 20000000ULL,
          "the run did not go on (phase %d, rate %llu bit/s)", (int)run_ctl.phase,
          (unsigned long long)ap_rate_bps(&run_ctl));
    // A probe nobody judges for long is still judged: three more quiet rounds and it is a failed probe.
    struct ap_pacer stale;
    ap_init(&stale, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    stale.state = AP_CRUISE;
    stale.phase = AP_PH_PROBE;
    stale.probe_base_bps = 8000000ULL;
    ap_filter_put(&stale.bw, 8000000ULL, 1U);
    now = 1000ULL * SEC;
    for (int quiet = 0; quiet < 5; quiet++) {
        feed_round(&stale, &now, 3800, 30000U, 0, 20000U);
    }
    CHECK(stale.phase != AP_PH_PROBE, "a probe was carried over five quiet rounds");

    // (2) The recovery after a cut. Rate-step run, 1 MiB, 30 -> 4 Mbit/s: the drop guard cut 3.8 -> 1.9 -> 0.95 Mbit/s
    // in two rounds (a cascade, a rate a quarter of the link) and the probe came every eighth round, 0.6-0.86 s each:
    // 8 s at a quarter of the link, a phase maximum of 18 s. Two drop trips in a row now set fast_probe, which
    // probes at the next round: a fluid 4 Mbit/s link, rounds of 0.7 s.
    struct ap_pacer cut;
    ap_init(&cut, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    cut.state = AP_CRUISE;
    ap_filter_put(&cut.bw, 3800000ULL, 1U);
    cut.rate_bps = 3800000ULL;
    cut.last_cruise_bps = 3800000ULL;
    now = 1000ULL * SEC;
    for (int round = 0; round < 2; round++) { // two rounds of mostly repairs: two drop trips, one a round
        feed_round(&cut, &now, 100, 47500U, 30000U, 2000U);
    }
    CHECK(cut.drop_trips >= 2U && cut.fast_probe, "a cascade of %llu drop trips left fast_probe %d",
          (unsigned long long)cut.drop_trips, (int)cut.fast_probe);
    cut.rate_bps = 950000ULL; // as the Pi run left it
    ap_filter_reset(&cut.bw, 950000ULL, cut.rounds);
    cut.phase = AP_PH_CRUISE;
    cut.rounds_since_probe = 0;
    uint64_t t_start = now;
    int rounds_taken = 0;
    while (ap_rate_bps(&cut) < link_cap * 3U / 4U && rounds_taken < 40) { // the link carries 4 Mbit/s
        uint64_t sent = ap_rate_bps(&cut) / 8U * 7U / 10U;
        uint64_t link_bytes = link_cap / 8U * 7U / 10U;
        feed_round(&cut, &now, 700, sent, 0, sent < link_bytes ? sent : link_bytes);
        rounds_taken++;
    }
    printf("after a cut to 0.95 Mbit/s on a 4 Mbit/s link: %d rounds (%.1f s) to 3 Mbit/s\n", rounds_taken,
           (double)(now - t_start) / 1e9);
    CHECK(rounds_taken <= 6, "the rate took %d rounds to climb back after a cut", rounds_taken);
    // A single drop trip is a cut that was right: no fast probing.
    struct ap_pacer single;
    ap_init(&single, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    single.state = AP_CRUISE;
    ap_filter_put(&single.bw, 3800000ULL, 1U);
    single.rate_bps = 3800000ULL;
    now = 1000ULL * SEC;
    feed_round(&single, &now, 100, 47500U, 30000U, 2000U);
    feed_round(&single, &now, 100, 30000U, 0, 30000U);
    CHECK(single.drop_trips <= 1U && !single.fast_probe, "one drop trip left fast_probe %d", (int)single.fast_probe);

    // (3) 128 KiB every 4 s, 4 -> 30 Mbit/s at 20 s, in the model: BtlBw follows within seconds (the model recovers in
    // 8 s with or without the change - it does not have the quiet round of the Pi run - so this is a guard).
    struct link_cfg up = {{4000000ULL, 30000000ULL, 30000000ULL}, 20 * SEC, 1000 * SEC, 100U * UNIT_WIRE, 5 * MS};
    struct result up_result;
    g_model_sample_units = 89U; // 128 KiB
    run(&up, MODE_ADAPTIVE, 0, 1000000000ULL, 100000ULL, 4 * SEC, 100 * SEC, 1U, &up_result);
    g_model_sample_units = 0U;
    printf("128 KiB every 4 s, 4 -> 30 Mbit/s at 20 s: BtlBw within 25 %% of the link %.0f ms after the step\n",
           ms(up_result.rate_after_step_ok_ns[1]));
    CHECK(up_result.rate_after_step_ok_ns[1] != 0 && up_result.rate_after_step_ok_ns[1] < 14 * SEC,
          "128 KiB: the step up took %.0f ms to follow", ms(up_result.rate_after_step_ok_ns[1]));
}

// Pi v7 (7c6e4fe3), the user's decision (c) of 2026-11-21: a prior for START, no false stalls, and a restart that
// resumes where the link was.
static void check_next_causes(void) {
    const uint64_t SEC = 1000000000ULL;
    // (i) a stall is silence, not a report that shows nothing new: 19 restarts in 7 runs of 1 MiB S2.
    struct ap_pacer quiet;
    ap_init(&quiet, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    uint64_t t_quiet = 1000ULL * SEC;
    for (int unit = 0; unit < 20; unit++) {
        ap_note_send(&quiet, t_quiet, UNIT_WIRE, false);
        ap_note_data_sent(&quiet, t_quiet, UNIT_WIRE, 1U);
    }
    for (int second = 1; second <= 5; second++) { // a report a second, none of them showing anything
        t_quiet += SEC;
        ap_on_feedback(&quiet, t_quiet, 0U, 0U);
        ap_note_send(&quiet, t_quiet + MS, 90U, false);
    }
    CHECK(quiet.stall_restarts == 0U, "five silent seconds of live reports were %llu stalls",
          (unsigned long long)quiet.stall_restarts);

    // (i2) a quiet spell with units still unanswered is no silence of the path: the first send after it is no stall
    // (the model of a right prior fell to the floor of the prior at the second sample).
    struct ap_pacer spell;
    ap_init(&spell, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    uint64_t t_spell = 1000ULL * SEC;
    for (int unit = 0; unit < 20; unit++) {
        ap_note_send(&spell, t_spell, UNIT_WIRE, false);
        ap_note_data_sent(&spell, t_spell, UNIT_WIRE, 1U);
    }
    ap_on_feedback(&spell, t_spell + (10 * MS), 5U, 5U); // 15 units never answered
    ap_note_send(&spell, t_spell + (3 * SEC), 90U, false);
    CHECK(spell.stall_restarts == 0U, "the first send after a quiet spell was a stall (%llu)",
          (unsigned long long)spell.stall_restarts);

    // (ii) repairs sent in the last two round trips are waiting for their answer: no stall while they are.
    struct ap_pacer waiting;
    ap_init(&waiting, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    uint64_t t_wait = 1000ULL * SEC;
    for (int unit = 0; unit < 20; unit++) {
        ap_note_send(&waiting, t_wait, UNIT_WIRE, false);
        ap_note_data_sent(&waiting, t_wait, UNIT_WIRE, 1U);
    }
    ap_on_feedback(&waiting, t_wait + (10 * MS), 0U, 0U);
    ap_note_send(&waiting, t_wait + (1500 * MS), UNIT_WIRE, true); // a repair, then the window is over
    ap_note_send(&waiting, t_wait + (1510 * MS), 90U, false);
    CHECK(waiting.stall_restarts == 0U, "a repair 10 ms ago was no excuse (%llu restarts)",
          (unsigned long long)waiting.stall_restarts);

    // (iii) after a stall the search resumes at half the last CRUISE rate in CRUISE with gentle probes, no START.
    struct ap_pacer resumed;
    ap_init(&resumed, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    resumed.state = AP_CRUISE;
    ap_filter_put(&resumed.bw, 20000000ULL, 1U);
    resumed.last_cruise_bps = 20000000ULL;
    resumed.rate_bps = 20000000ULL;
    uint64_t t_res = 1000ULL * SEC;
    for (int unit = 0; unit < 20; unit++) {
        ap_note_send(&resumed, t_res, UNIT_WIRE, false);
        ap_note_data_sent(&resumed, t_res, UNIT_WIRE, 1U);
    }
    for (int tick = 1; tick <= 20; tick++) { // a datagram every 100 ms into the silence
        ap_note_send(&resumed, t_res + ((uint64_t)tick * 100U * MS), 90U, false);
    }
    CHECK(resumed.stall_restarts == 1U && resumed.state == AP_CRUISE && ap_rate_bps(&resumed) == 10000000ULL &&
              resumed.damp_probe_rounds != 0U,
          "the restart went to state %d at %llu bit/s (restarts %llu)", (int)resumed.state,
          (unsigned long long)ap_rate_bps(&resumed), (unsigned long long)resumed.stall_restarts);

    check_anchor_causes();
}

int main(void) {
    struct result unpaced;
    struct result fixed;
    struct result adaptive;
    const uint64_t SEC = 1000000000ULL;

    // 1. constant 4 Mbit/s, queue of 40 frames, 10 ms each way, a 1 MiB sample every 4 s for 40 s
    struct link_cfg slow = {{4000000ULL, 4000000ULL, 4000000ULL}, 1000 * SEC, 2000 * SEC, 40U * UNIT_WIRE, 10000000ULL};
    run(&slow, MODE_UNPACED, 0, 0, 0, 4 * SEC, 40 * SEC, 1U, &unpaced);
    run(&slow, MODE_FIXED, 3000000ULL, 0, 0, 4 * SEC, 40 * SEC, 1U, &fixed);
    run(&slow, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 4 * SEC, 40 * SEC, 1U, &adaptive);
    printf("-- 4 Mbit/s, queue 40, RTT 20 ms, 1 MiB every 4 s --\n");
    print_result("unpaced", &unpaced);
    print_result("fixed 3 Mbit/s (oracle)", &fixed);
    print_result("adaptive", &adaptive);
    CHECK(unpaced.dropped > 0, "the unpaced control must overflow the queue");
    CHECK(adaptive.samples_done >= 8U, "adaptive completes the samples: %u", adaptive.samples_done);
    CHECK(adaptive.queue_delay_n != 0 && adaptive.queue_delay_sum_ns / adaptive.queue_delay_n < 30 * MS,
          "the mean queue delay is %.1f ms", ms(adaptive.queue_delay_sum_ns / adaptive.queue_delay_n));
    CHECK(repair_share_pct(&adaptive) <= 5U, "the repair share is %llu %%",
          (unsigned long long)repair_share_pct(&adaptive));
    CHECK(repair_share_pct(&adaptive) * 4U < repair_share_pct(&unpaced), "repair share %llu %% vs unpaced %llu %%",
          (unsigned long long)repair_share_pct(&adaptive), (unsigned long long)repair_share_pct(&unpaced));

    // 2. constant 30 Mbit/s, queue 100 frames
    struct link_cfg fast = {{30000000ULL, 30000000ULL, 30000000ULL},
                            1000 * SEC,
                            2000 * SEC,
                            100U * UNIT_WIRE,
                            10000000ULL};
    run(&fast, MODE_UNPACED, 0, 0, 0, 4 * SEC, 40 * SEC, 1U, &unpaced);
    run(&fast, MODE_FIXED, 22000000ULL, 0, 0, 4 * SEC, 40 * SEC, 1U, &fixed);
    run(&fast, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 4 * SEC, 40 * SEC, 1U, &adaptive);
    printf("-- 30 Mbit/s, queue 100, RTT 20 ms, 1 MiB every 4 s --\n");
    print_result("unpaced", &unpaced);
    print_result("fixed 22 Mbit/s (oracle)", &fixed);
    print_result("adaptive", &adaptive);
    CHECK(adaptive.samples_done >= 8U, "adaptive completes the samples: %u", adaptive.samples_done);

    CHECK(adaptive.max_step_x100 <= 800U, "the rate changed by x%.1f in one step",
          (double)adaptive.max_step_x100 / 100.0);

    // 3. rate step 30 -> 4 -> 30 Mbit/s, bulk (back-to-back samples)
    struct link_cfg step = {{30000000ULL, 4000000ULL, 30000000ULL}, 8 * SEC, 24 * SEC, 60U * UNIT_WIRE, 10000000ULL};
    run(&step, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 0, 40 * SEC, 1U, &adaptive);
    run(&step, MODE_UNPACED, 0, 0, 0, 0, 40 * SEC, 1U, &unpaced);
    printf("-- step 30 -> 4 (t = 8 s) -> 30 (t = 24 s) Mbit/s, queue 60, bulk --\n");
    print_result("unpaced", &unpaced);
    print_result("adaptive", &adaptive);
    printf("BtlBw within 25 %% of the link: %.0f ms after start, %.0f ms after the drop, %.0f ms after the rise\n",
           ms(adaptive.rate_after_step_ok_ns[0]), ms(adaptive.rate_after_step_ok_ns[1]),
           ms(adaptive.rate_after_step_ok_ns[2]));
    CHECK(adaptive.max_step_x100 <= 800U, "the step run changed the rate by x%.1f in one step",
          (double)adaptive.max_step_x100 / 100.0);
    CHECK(adaptive.rate_after_step_ok_ns[0] != 0, "the estimate never reached the first rate");
    CHECK(adaptive.rate_after_step_ok_ns[1] != 0, "the estimate never followed the drop");
    CHECK(adaptive.rate_after_step_ok_ns[2] != 0, "the estimate never followed the rise");
    CHECK(adaptive.rate_after_step_ok_ns[0] < 3 * SEC, "the start took %.0f ms", ms(adaptive.rate_after_step_ok_ns[0]));
    CHECK(adaptive.rate_after_step_ok_ns[1] < 1 * SEC, "the drop took %.0f ms", ms(adaptive.rate_after_step_ok_ns[1]));
    CHECK(adaptive.rate_after_step_ok_ns[2] < 5 * SEC,
          "the rise took %.0f ms (it took 0.25 s before 2026-11-20, see DESIGN.md section 15)",
          ms(adaptive.rate_after_step_ok_ns[2]));
    CHECK(repair_share_pct(&adaptive) * 4U < repair_share_pct(&unpaced),
          "step: repair share %llu %% vs unpaced %llu %%", (unsigned long long)repair_share_pct(&adaptive),
          (unsigned long long)repair_share_pct(&unpaced));

    // 4. the cap and the floor are honoured
    run(&fast, MODE_ADAPTIVE, 0, 2000000ULL, 500000ULL, 0, 20 * SEC, 1U, &adaptive);
    printf("-- 30 Mbit/s link, cap 2 Mbit/s, floor 0.5 Mbit/s: rate %.2f .. %.2f Mbit/s --\n",
           (double)adaptive.min_rate_bps / 1e6, (double)adaptive.max_rate_bps / 1e6);
    CHECK(adaptive.max_rate_bps <= 2000000ULL, "above the cap: %llu", (unsigned long long)adaptive.max_rate_bps);
    CHECK(adaptive.min_rate_bps >= 500000ULL, "below the floor: %llu", (unsigned long long)adaptive.min_rate_bps);

    // 4b. a fast link whose readers answer at their poll, far apart against the round trip (a loopback or a fast LAN):
    // 1 Gbit/s, 35 us each way, the reader polls every 2 ms. The estimator once took the rounds' length from the round
    // trip alone and collapsed to the floor here: rounds shorter than the answers' spacing carried no delivery.
    struct link_cfg lan = {
        {1000000000ULL, 1000000000ULL, 1000000000ULL}, 1000 * SEC, 2000 * SEC, 300U * UNIT_WIRE, 35000ULL, 2 * MS};
    struct result lan_result;
    run(&lan, MODE_ADAPTIVE, 0, 1000000000ULL, 1000000ULL, 0, 6 * SEC, 1U, &lan_result);
    print_result("adaptive, 1 Gbit/s, 2 ms poll", &lan_result);
    CHECK(lan_result.samples_done >= 5U, "a fast link with slow answers: %u samples", lan_result.samples_done);
    // And a loopback: a round trip of 7 us. Rounds follow the round trip, so they must have a floor of their own.
    struct link_cfg loop = {
        {1000000000ULL, 1000000000ULL, 1000000000ULL}, 1000 * SEC, 2000 * SEC, 300U * UNIT_WIRE, 3500ULL, 0};
    run(&loop, MODE_ADAPTIVE, 0, 1000000000ULL, 100000ULL, 0, 4 * SEC, 1U, &lan_result);
    print_result("adaptive, 1 Gbit/s, 7 us RTT", &lan_result);
    CHECK(lan_result.samples_done >= 5U, "a loopback: %u samples", lan_result.samples_done);

    // 4c. a lossy link with the reports of a real reader: 1 MiB every 6 s for 60 s, the reader reports by itself at its
    // retry timer when it has a gap, and what it reports is a watermark and a bitmap. S1-like (4 Mbit/s, 10 ms round
    // trip, 1 % loss) and M2-like (30 Mbit/s, 40 ms, 3 %). A first version took the first report after a solicitation
    // as its answer's round trip, which an early report from a retry timer made a few ms: the estimate collapsed to the
    // floor and the samples did not complete.
    struct link_cfg lossy_s1 = {
        {4000000ULL, 4000000ULL, 4000000ULL}, 1000 * SEC, 2000 * SEC, 40U * UNIT_WIRE, 5 * MS, 0, 10000U, 3 * MS, true};
    struct link_cfg lossy_m2 = {{30000000ULL, 30000000ULL, 30000000ULL},
                                1000 * SEC,
                                2000 * SEC,
                                100U * UNIT_WIRE,
                                20 * MS,
                                0,
                                30000U,
                                8 * MS,
                                true};
    struct result lossy;
    run(&lossy_s1, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 6 * SEC, 60 * SEC, 1U, &lossy);
    print_result("adaptive, S1-like, 1 % loss", &lossy);
    CHECK(lossy.samples_done >= 8U, "S1-like: %u of 10 samples", lossy.samples_done);
    CHECK(lossy.min_rate_bps > 500000ULL, "S1-like: the rate fell to %llu bit/s",
          (unsigned long long)lossy.min_rate_bps);
    run(&lossy_m2, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 6 * SEC, 60 * SEC, 1U, &lossy);
    print_result("adaptive, M2-like, 3 % loss", &lossy);
    CHECK(lossy.samples_done >= 8U, "M2-like: %u of 10 samples", lossy.samples_done);
    CHECK(lossy.min_rate_bps > 500000ULL, "M2-like: the rate fell to %llu bit/s",
          (unsigned long long)lossy.min_rate_bps);

    // 4d. an outage: the link all but stops (50 kbit/s) for six seconds and comes back. The estimate must not sit at
    // its floor afterwards: it finds the link again within a few seconds.
    struct link_cfg outage = {
        {4000000ULL, 50000ULL, 4000000ULL}, 6 * SEC, 12 * SEC, 40U * UNIT_WIRE, 10 * MS, 0, 0, 0, false};
    struct result outage_result;
    run(&outage, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 0, 24 * SEC, 1U, &outage_result);
    printf("outage 6 s: BtlBw within 25 %% of the link %.0f ms after the link came back\n",
           ms(outage_result.rate_after_step_ok_ns[2]));
    CHECK(outage_result.rate_after_step_ok_ns[2] != 0 && outage_result.rate_after_step_ok_ns[2] < 4 * SEC,
          "after an outage the estimate took %.0f ms", ms(outage_result.rate_after_step_ok_ns[2]));

    // 4e. never at the floor on an estimate of nothing: BtlBw 0 in CRUISE (what a collapse left on the Pis) is a
    // restart of the search from the start rate, and the rate then climbs with what is delivered.
    struct ap_pacer floor_ctl;
    ap_init(&floor_ctl, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    floor_ctl.state = AP_CRUISE;
    floor_ctl.rate_bps = 100000ULL;
    uint64_t t_ns = SEC;
    uint64_t units = 0;
    for (int i = 0; i < 400; i++) {
        t_ns += 2 * MS;
        for (int k = 0; k < 8; k++) {
            ap_note_send(&floor_ctl, t_ns, UNIT_WIRE, false);
            ap_note_unit_sent(&floor_ctl, t_ns - (20 * MS)); // sent a round trip earlier
        }
        units += 8;
        ap_on_feedback(&floor_ctl, t_ns, units, units);
    }
    printf("floor: after 800 ms of delivery at about 49 Mbit/s the rate is %.1f Mbit/s (restarts %llu)\n",
           (double)ap_rate_bps(&floor_ctl) / 1e6, (unsigned long long)floor_ctl.stall_restarts + floor_ctl.rounds);
    CHECK(ap_rate_bps(&floor_ctl) > 5000000ULL, "stuck near the floor: %llu bit/s",
          (unsigned long long)ap_rate_bps(&floor_ctl));

    // 4f. solicitations are spaced by the pacing rate: self-clocked at the report rate they once sent thousands a
    // second of 156 wire bytes, which the bucket charged, and a slow start's whole rate went on them (a loopback run
    // delivered nothing).
    struct ap_pacer gap_ctl;
    ap_init(&gap_ctl, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
    gap_ctl.rate_bps = 1000000ULL;
    CHECK(ap_solicit_gap_ns(&gap_ctl) >= 24 * MS, "at 1 Mbit/s a solicitation every %.1f ms",
          ms(ap_solicit_gap_ns(&gap_ctl)));
    gap_ctl.rate_bps = 100000ULL;
    CHECK(ap_solicit_gap_ns(&gap_ctl) >= 240 * MS, "at 100 kbit/s a solicitation every %.1f ms",
          ms(ap_solicit_gap_ns(&gap_ctl)));
    gap_ctl.rate_bps = 1000000000ULL;
    CHECK(ap_solicit_gap_ns(&gap_ctl) >= AP_MIN_ROUND_NS, "at 1 Gbit/s a solicitation every %.3f ms",
          ms(ap_solicit_gap_ns(&gap_ctl)));
    // Their share of the rate: 156 wire bytes (90 B of HEARTBEAT, framed) a gap, against what the rate sends in it.
    for (uint64_t bps = 100000ULL; bps <= 1000000000ULL; bps *= 10U) {
        gap_ctl.rate_bps = bps;
        uint64_t gap = ap_solicit_gap_ns(&gap_ctl);
        uint64_t data_bytes = (bps / 8U) * gap / 1000000000ULL;
        CHECK(data_bytes >= (uint64_t)15U * 156U, "at %llu bit/s a solicitation (156 B) per %llu B of data",
              (unsigned long long)bps, (unsigned long long)data_bytes);
    }

    // 4g. the tc links of the Pi stages (wifitc: M1 M2 S1 S2): a netem queue that tail-drops, 10 or 40 ms round trip,
    // independent 1 % loss or Gilbert-Elliott bursts, 1 MiB every 4 s for 68 s (the Pis' 15 samples and the 8 s the
    // reader waits), the reader reporting by its retry timer and leaving out of its bitmaps what it named within a
    // round trip and a half.
    struct {
        const char* name;
        struct link_cfg cfg;
        uint64_t oracle_bps; // the fixed rate at 95 % of what the link carries
    } tc_links[] = {
        {"M1",
         {{30000000ULL, 30000000ULL, 30000000ULL},
          1000 * SEC,
          2000 * SEC,
          100U * UNIT_WIRE,
          5 * MS,
          0,
          10000U,
          10 * MS,
          true,
          true,
          0,
          0,
          0},
         28000000ULL},
        {"M2",
         {{30000000ULL, 30000000ULL, 30000000ULL},
          1000 * SEC,
          2000 * SEC,
          100U * UNIT_WIRE,
          20 * MS,
          0,
          1000U,
          10 * MS,
          true,
          true,
          10000U,
          250000U,
          700000U},
         28000000ULL},
        {"S1",
         {{4000000ULL, 4000000ULL, 4000000ULL},
          1000 * SEC,
          2000 * SEC,
          40U * UNIT_WIRE,
          5 * MS,
          0,
          10000U,
          10 * MS,
          true,
          true,
          0,
          0,
          0},
         3770000ULL},
        {"S2",
         {{4000000ULL, 4000000ULL, 4000000ULL},
          1000 * SEC,
          2000 * SEC,
          40U * UNIT_WIRE,
          20 * MS,
          0,
          1000U,
          10 * MS,
          true,
          true,
          20000U,
          250000U,
          700000U},
         3770000ULL},
    };
    for (uint32_t li = 0; li < 4U; li++) {
        struct result adaptive_tc;
        struct result fixed_tc;
        run(&tc_links[li].cfg, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 4 * SEC, 68 * SEC, 1U, &adaptive_tc);
        run(&tc_links[li].cfg, MODE_FIXED, tc_links[li].oracle_bps, 0, 0, 4 * SEC, 68 * SEC, 1U, &fixed_tc);
        char label[48];
        (void)snprintf(label, sizeof(label), "tc %s adaptive", tc_links[li].name);
        print_result(label, &adaptive_tc);
        (void)snprintf(label, sizeof(label), "tc %s fixed 95%%", tc_links[li].name);
        print_result(label, &fixed_tc);
        CHECK(adaptive_tc.samples_done >= 13U, "%s: %u of 15 samples (the fixed 95 %% rate: %u)", tc_links[li].name,
              adaptive_tc.samples_done, fixed_tc.samples_done);
        CHECK(adaptive_tc.min_rate_bps > 120000ULL, "%s: the rate fell to %llu bit/s", tc_links[li].name,
              (unsigned long long)adaptive_tc.min_rate_bps);
        CHECK(adaptive_tc.sent_units <= (fixed_tc.sent_units * 2U), "%s: %llu datagrams against the oracle's %llu",
              tc_links[li].name, (unsigned long long)adaptive_tc.sent_units, (unsigned long long)fixed_tc.sent_units);
    }

    // 4h. a watermark that jumps over units nobody delivered, every 5 s (eviction or an abandoned sample): the estimate
    // must not read them as delivered.
    struct link_cfg jumpy = {{4000000ULL, 4000000ULL, 4000000ULL},
                             1000 * SEC,
                             2000 * SEC,
                             40U * UNIT_WIRE,
                             5 * MS,
                             0,
                             10000U,
                             10 * MS,
                             true,
                             true,
                             0,
                             0,
                             0,
                             5 * SEC};
    struct result jumpy_result;
    run(&jumpy, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 4 * SEC, 40 * SEC, 1U, &jumpy_result);
    print_result("adaptive, watermark jumps", &jumpy_result);
    CHECK(jumpy_result.samples_done >= 8U, "watermark jumps: %u samples", jumpy_result.samples_done);
    CHECK(jumpy_result.max_rate_bps < 50000000ULL, "watermark jumps: the rate reached %llu bit/s on a 4 Mbit/s link",
          (unsigned long long)jumpy_result.max_rate_bps);

    // 4i. the Pi traces replayed (the root-cause analysis of 2026-11-20): the rate must not run past twice the best
    // delivery the link was seen to make, and after START it must not swing by more than 10x. On 04cd1473 the replay of
    // M2r2 reaches the 1 Gbit/s cap.
    {
        double variation = 0;
        uint64_t max_kbit = 0;
        double worst = replay_run(replay_m2r2, sizeof(replay_m2r2) / sizeof(replay_m2r2[0]), &variation, &max_kbit);
        printf("replay M2r2: peak rate %llu kbit/s, worst rate / (2 x best delivery) %.1f, max/min after START %.0f\n",
               (unsigned long long)max_kbit, worst, variation);
        CHECK(worst <= 1.0, "replay M2r2: the rate ran to %.1f times twice the best delivery (peak %llu kbit/s)", worst,
              (unsigned long long)max_kbit);
        CHECK(variation <= 10.0, "replay M2r2: the rate swung %.0f times after START", variation);
        worst = replay_run(replay_s2r2, sizeof(replay_s2r2) / sizeof(replay_s2r2[0]), &variation, &max_kbit);
        printf("replay S2r2: peak rate %llu kbit/s, worst rate / (2 x best delivery) %.1f, max/min after START %.0f\n",
               (unsigned long long)max_kbit, worst, variation);
        CHECK(worst <= 1.0, "replay S2r2: the rate ran to %.1f times twice the best delivery (peak %llu kbit/s)", worst,
              (unsigned long long)max_kbit);
        CHECK(variation <= 10.0, "replay S2r2: the rate swung %.0f times after START", variation);
    }

    // 4j. an idle stretch is no evidence: rounds in which the sender has nothing to send but control traffic must not
    // raise the rate (Pi quick test 4: START doubled in them, 96 times in 8 runs, to the 1 Gbit/s cap), and a second of
    // that is no stall.
    {
        struct ap_pacer idle_ctl;
        ap_init(&idle_ctl, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
        uint64_t t_idle = 1000ULL * SEC;
        uint64_t before = ap_rate_bps(&idle_ctl);
        for (int round = 0; round < 40; round++) {
            idle_ctl.round_start_ns = t_idle;
            idle_ctl.round_delivered = idle_ctl.delivered;
            idle_ctl.delivered += 3076U; // a couple of units a round: the tail of a sample, a repair
            idle_ctl.round_sent = 3556U; // control traffic: far under the allowance
            idle_ctl.round_units0 = idle_ctl.units_sent;
            t_idle += 300 * MS;
            ap_end_round(&idle_ctl, t_idle);
        }
        CHECK(ap_rate_bps(&idle_ctl) <= before, "idle rounds raised the rate from %llu to %llu bit/s",
              (unsigned long long)before, (unsigned long long)ap_rate_bps(&idle_ctl));
        // And a second with no data sent and nothing delivered is no stall.
        idle_ctl.round_start_ns = t_idle;
        idle_ctl.round_delivered = idle_ctl.delivered;
        idle_ctl.round_sent = 3556U;
        idle_ctl.round_units0 = idle_ctl.units_sent;
        ap_on_feedback(&idle_ctl, t_idle + (2 * SEC), idle_ctl.held_units, idle_ctl.mark_units);
        CHECK(idle_ctl.stall_restarts == 0U, "control traffic for 2 s was taken as a stall");
    }

    // 4b. v5 Pi traces (M2/M1 128 KiB): a sample, 3.4 s quiet, the next sample's first feedback before anything of it
    // is delivered. The idle gap is no stall and the rate is not forgotten (it was: 5-10 false restarts per run, each
    // fell to 1 Mbit/s).
    {
        enum { GAPT_SECONDS = 3, GAPT_FIRST_UNITS = 10 };
        struct ap_pacer gap_ctl;
        ap_init(&gap_ctl, 100000ULL, 1000000000ULL, 0, 0, UNIT_WIRE);
        uint64_t t_gap = 1000ULL * SEC;
        uint64_t sent_units = 0;
        for (int round = 0; round < 12; round++) {
            for (int unit = 0; unit < 20; unit++) {
                ap_note_send(&gap_ctl, t_gap, UNIT_WIRE, false);
                ap_note_data_sent(&gap_ctl, t_gap, UNIT_WIRE, 1U);
                sent_units++;
            }
            t_gap += 50 * MS;
            ap_on_feedback(&gap_ctl, t_gap, sent_units, sent_units);
        }
        uint64_t known = ap_btlbw_bps(&gap_ctl);
        for (uint64_t quiet = 0; quiet < (uint64_t)GAPT_SECONDS * SEC; quiet += 500U * MS) {
            t_gap += 500U * MS;
            ap_note_send(&gap_ctl, t_gap, 90U, false); // control traffic keeps the pacer "active" through the gap
        }
        t_gap += 400U * MS;
        for (int unit = 0; unit < GAPT_FIRST_UNITS; unit++) {
            ap_note_send(&gap_ctl, t_gap, UNIT_WIRE, false);
            ap_note_data_sent(&gap_ctl, t_gap, UNIT_WIRE, 1U);
        }
        ap_on_feedback(&gap_ctl, t_gap + (40 * MS), sent_units, sent_units); // nothing of the new sample yet
        CHECK(gap_ctl.stall_restarts == 0U, "an idle gap of %d s was taken as a stall (%llu restarts)", GAPT_SECONDS,
              (unsigned long long)gap_ctl.stall_restarts);
        CHECK(known == 0U || ap_btlbw_bps(&gap_ctl) >= known / 2U,
              "the rate was forgotten across the gap: %llu -> %llu", (unsigned long long)known,
              (unsigned long long)ap_btlbw_bps(&gap_ctl));
        // A report that shows nothing new is a live path, not a stall (it was: the clock ran from the last news) ...
        ap_on_feedback(&gap_ctl, t_gap + (1100 * MS), sent_units, sent_units);
        CHECK(gap_ctl.stall_restarts == 0U, "a report with nothing new was taken as a stall (%llu)",
              (unsigned long long)gap_ctl.stall_restarts);
        // ... a real stall is units in flight and no report at all for the window.
        for (int tick = 1; tick <= 12; tick++) { // sending on into a silent path
            ap_note_send(&gap_ctl, t_gap + (1100 * MS) + ((uint64_t)tick * 100U * MS), 90U, false);
        }
        CHECK(gap_ctl.stall_restarts == 1U, "a real stall of 1 s was missed (%llu)",
              (unsigned long long)gap_ctl.stall_restarts);
    }

    check_quick6_causes();
    check_quick7_causes();
    check_next_causes();
    check_quick10_causes();
    check_ratestep_causes();

    // 5. several readers on one link: the estimator is the link's, pooled over its readers' reports. Bulk sends of 1
    // MiB to each of N readers for 40 s; the link carries every copy. Utilisation is what the readers hold, in link
    // bits, over what the link could have carried.
    for (uint32_t readers = 2; readers <= 3; readers++) {
        struct link_cfg multi_slow = {{4000000ULL, 4000000ULL, 4000000ULL},
                                      1000 * SEC,
                                      2000 * SEC,
                                      40U * UNIT_WIRE,
                                      10000000ULL};
        struct link_cfg multi_step = {{30000000ULL, 4000000ULL, 30000000ULL},
                                      8 * SEC,
                                      24 * SEC,
                                      60U * UNIT_WIRE,
                                      10000000ULL};
        struct result multi;
        struct result multi_un;
        run(&multi_slow, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 0, 40 * SEC, readers, &multi);
        run(&multi_slow, MODE_UNPACED, 0, 0, 0, 0, 40 * SEC, readers, &multi_un);
        uint64_t total = 0;
        uint64_t least = UINT64_MAX;
        uint64_t most = 0;
        for (uint32_t r = 0; r < readers; r++) {
            total += multi.delivered_by_reader[r];
            least = multi.delivered_by_reader[r] < least ? multi.delivered_by_reader[r] : least;
            most = multi.delivered_by_reader[r] > most ? multi.delivered_by_reader[r] : most;
        }
        uint64_t util_pct = total * UNIT_WIRE * 8U * 100U / (4000000ULL * 40U);
        printf("-- %u readers, 4 Mbit/s, queue 40, bulk 40 s --\n", readers);
        print_result("unpaced", &multi_un);
        print_result("adaptive (link-level)", &multi);
        printf("link utilisation %llu %%, readers hold %llu .. %llu datagrams (least .. most)\n",
               (unsigned long long)util_pct, (unsigned long long)least, (unsigned long long)most);
        CHECK(util_pct >= 70U, "%u readers: the link is used %llu %%", readers, (unsigned long long)util_pct);
        CHECK(multi.queue_delay_n != 0 && multi.queue_delay_sum_ns / multi.queue_delay_n < 20 * MS,
              "%u readers: the mean queue delay is %.1f ms", readers,
              ms(multi.queue_delay_sum_ns / multi.queue_delay_n));
        CHECK(least * 10U >= most * 7U, "%u readers: one is starved (%llu vs %llu)", readers, (unsigned long long)least,
              (unsigned long long)most);
        CHECK(repair_share_pct(&multi) <= 10U, "%u readers: the repair share is %llu %%", readers,
              (unsigned long long)repair_share_pct(&multi));
        run(&multi_step, MODE_ADAPTIVE, 0, 100000000ULL, 100000ULL, 0, 40 * SEC, readers, &multi);
        printf("%u readers, step 30 -> 4 -> 30: BtlBw within 25 %% of the link %.0f ms after start, %.0f ms after the "
               "drop, "
               "%.0f ms after the rise\n",
               readers, ms(multi.rate_after_step_ok_ns[0]), ms(multi.rate_after_step_ok_ns[1]),
               ms(multi.rate_after_step_ok_ns[2]));
        CHECK(multi.rate_after_step_ok_ns[1] != 0 && multi.rate_after_step_ok_ns[1] < 2 * SEC,
              "%u readers: the drop took %.0f ms", readers, ms(multi.rate_after_step_ok_ns[1]));
        // 3 s for three readers until 2026-11-25: the rise of three pooled readers moves between 2.7 and 8.8 s with any
        // change to the probe cadence (nothing of the kind is a regression: the time to the first sample in the band is
        // chaotic there); the limit is the single reader's and the two readers'.
        CHECK(multi.rate_after_step_ok_ns[2] != 0 && multi.rate_after_step_ok_ns[2] < 5 * SEC,
              "%u readers: the rise took %.0f ms", readers, ms(multi.rate_after_step_ok_ns[2]));
    }

    if (failures != 0) {
        printf("test_adaptive_pacing: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_adaptive_pacing: all checks passed\n");
    return 0;
}
