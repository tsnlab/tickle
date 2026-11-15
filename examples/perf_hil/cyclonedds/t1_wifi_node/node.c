/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// CycloneDDS node of the T1 framework comparison (roles and semantics: t1fw_common.h). The participant is configured
// by CYCLONEDDS_URI (the harness sets the interface and SPDPInterval); nothing is tuned here.
#include <signal.h>

#include <dds/dds.h>

#include "Blob.h"
#include "t1fw_common.h"

static struct t1_opts opt;
static struct t1_score sc;
static dds_entity_t part, wr, rd, ws;
static uint8_t* txbuf;

static dds_qos_t* make_qos(void) {
    dds_qos_t* q = dds_create_qos();
    dds_qset_reliability(q, opt.reliable ? DDS_RELIABILITY_RELIABLE : DDS_RELIABILITY_BEST_EFFORT,
                         (dds_duration_t)(opt.block_s * 1e9));
    dds_qset_history(q, DDS_HISTORY_KEEP_LAST, (int32_t)opt.depth);
    return q;
}

static int send_sample(uint32_t len, uint64_t send_ns, uint32_t seq) {
    t1_hdr_put(txbuf, send_ns, seq);
    Blob msg;
    msg.data._maximum = len;
    msg.data._length = len;
    msg.data._buffer = txbuf;
    msg.data._release = false;
    return dds_write(wr, &msg) == DDS_RETCODE_OK ? 0 : -1;
}

// Takes everything waiting on the reader. answer: echo role, pong each first copy.
static void drain_reader(int answer, uint32_t* pongs_failed) {
    void* samples[16] = {NULL};
    dds_sample_info_t infos[16];
    int n;
    do {
        samples[0] = NULL;
        n = dds_take(rd, samples, infos, 16, 16);
        for (int i = 0; i < n; i++) {
            if (!infos[i].valid_data) {
                continue;
            }
            const Blob* m = (const Blob*)samples[i];
            if (m->data._length < T1_HDR) {
                continue;
            }
            uint64_t send_ns;
            uint32_t seq;
            t1_hdr_get(m->data._buffer, &send_ns, &seq);
            uint64_t now = t1_now_ns();
            if (t1_record(&sc, seq, send_ns, now) && answer) {
                if (send_sample(T1_HDR, send_ns, seq) != 0) {
                    (*pongs_failed)++;
                }
            }
        }
        if (n > 0) {
            dds_return_loan(rd, samples, n);
        }
    } while (n == 16);
}

static void service_until(uint64_t until, int answer, uint32_t* pf) {
    for (uint64_t n = t1_now_ns(); n < until && !t1_stop_asked(&opt); n = t1_now_ns()) {
        uint64_t left = until - n;
        dds_waitset_wait(ws, NULL, 0, (dds_duration_t)(left < 10000000ULL ? left : 10000000ULL));
        drain_reader(answer, pf);
    }
}

static int matched(dds_entity_t w, dds_entity_t r) {
    dds_publication_matched_status_t ps;
    dds_subscription_matched_status_t ss;
    int ok = 1;
    if (w > 0) {
        ok = ok && dds_get_publication_matched_status(w, &ps) == DDS_RETCODE_OK && ps.current_count > 0;
    }
    if (r > 0) {
        ok = ok && dds_get_subscription_matched_status(r, &ss) == DDS_RETCODE_OK && ss.current_count > 0;
    }
    return ok;
}

static int wait_match(dds_entity_t w, dds_entity_t r) {
    uint64_t deadline = t1_now_ns() + (uint64_t)(T1_MATCH_WAIT_S * 1e9);
    while (!matched(w, r) && t1_now_ns() < deadline) {
        t1_sleep_until(t1_now_ns() + 10000000ULL);
    }
    return matched(w, r);
}

int main(int argc, char** argv) {
    if (t1_parse(argc, argv, &opt) != 0) {
        fprintf(stderr, "usage: cyclonedds_node ping|echo|pub|sub [-d s] [-m be|rel] [-s B] [-r Hz] [-K depth] [-Q s] "
                        "[-B s] [-q stop] [-l file]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    uint64_t start = t1_now_ns();
    txbuf = calloc(1, opt.size > T1_HDR ? opt.size : T1_HDR);
    part = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (part < 0) {
        fprintf(stderr, "dds_create_participant: %s\n", dds_strretcode(-part));
        return 1;
    }
    dds_qos_t* q = make_qos();
    const char* wname = NULL;
    const char* rname = NULL;
    switch (opt.role) {
    case T1_PING:
        wname = "wifi_ping";
        rname = "wifi_pong";
        break;
    case T1_ECHO:
        wname = "wifi_pong";
        rname = "wifi_ping";
        break;
    case T1_PUB:
        wname = "wifi_ping";
        break;
    case T1_SUB:
        rname = "wifi_ping";
        break;
    }
    if (wname != NULL) {
        dds_entity_t t = dds_create_topic(part, &Blob_desc, wname, NULL, NULL);
        wr = dds_create_writer(part, t, q, NULL);
        if (wr < 0) {
            fprintf(stderr, "dds_create_writer: %s\n", dds_strretcode(-wr));
            return 1;
        }
    }
    if (rname != NULL) {
        dds_entity_t t = dds_create_topic(part, &Blob_desc, rname, NULL, NULL);
        rd = dds_create_reader(part, t, q, NULL);
        if (rd < 0) {
            fprintf(stderr, "dds_create_reader: %s\n", dds_strretcode(-rd));
            return 1;
        }
        ws = dds_create_waitset(part);
        dds_set_status_mask(rd, DDS_DATA_AVAILABLE_STATUS);
        dds_waitset_attach(ws, rd, 0);
    }
    dds_delete_qos(q);

    uint32_t published = 0;
    uint32_t failed = 0;
    int is_matched = 1;
    if (opt.role == T1_PING) {
        sc.warmup = (uint32_t)opt.rate_hz;
        is_matched = wait_match(wr, rd);
        if (is_matched) {
            uint64_t period = (uint64_t)(1e9 / opt.rate_hz);
            uint64_t end = t1_now_ns() + (uint64_t)(opt.seconds * 1e9);
            uint32_t pf = 0;
            for (uint64_t next = t1_now_ns(); next < end && published < T1_MAX_SEQ; next += period) {
                service_until(next, 0, &pf);
                if (send_sample(opt.size, t1_now_ns(), published) == 0) {
                    published++;
                } else {
                    failed++;
                }
            }
            service_until(t1_now_ns() + (uint64_t)(T1_DRAIN_S * 1e9), 0, &pf);
        }
    } else if (opt.role == T1_ECHO) {
        sc.warmup = (uint32_t)opt.rate_hz;
        uint32_t pf = 0;
        service_until(start + (uint64_t)(opt.seconds * 1e9), 1, &pf);
        failed = pf;
    } else if (opt.role == T1_PUB) {
        is_matched = wait_match(wr, 0);
        if (is_matched) {
            uint32_t n = (uint32_t)(opt.seconds * opt.rate_hz);
            uint64_t period = (uint64_t)(1e9 / opt.rate_hz);
            uint64_t next = t1_now_ns();
            uint64_t pub_t0 = next;
            for (uint32_t i = 0; i < n; i++, next += period) {
                t1_sleep_until(next);
                if (send_sample(opt.size, t1_now_ns(), i) == 0) {
                    published++;
                } else {
                    failed++;
                }
            }
            t1_sleep_until(pub_t0 + (uint64_t)((opt.seconds + opt.drain_s) * 1e9));
        }
    } else {
        uint32_t pf = 0;
        service_until(start + (uint64_t)(opt.seconds * 1e9), 0, &pf);
    }
    t1_result("cyclonedds", &opt, &sc, is_matched, published, failed, start, t1_now_ns(), NULL);
    return 0;
}
