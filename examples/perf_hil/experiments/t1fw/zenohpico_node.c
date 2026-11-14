/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// zenoh-pico 1.10.2 node of the T1 framework comparison (roles and semantics: t1fw_common.h). REFERENCE ONLY, never
// scored. Same transport rule as the repository's zenoh-pico harness (examples/perf_hil/zenohpico/latency_common/session.h):
// BEST_EFFORT over UDP multicast peer mode, RELIABLE over TCP (listen on the echo / sub side, connect from the ping / pub
// side; environment T1_ZENOH_LISTEN / T1_ZENOH_CONNECT, e.g. tcp/10.80.0.3:7447). zenoh-pico has no history QoS, no
// matched-endpoint API without listeners: the node settles 3 s after the session opens, as the repository's harness does.
#include <pthread.h>
#include <signal.h>
#include <zenoh-pico.h>

#include "t1fw_common.h"

static struct t1_opts opt;
static struct t1_score sc;
static pthread_mutex_t score_mutex = PTHREAD_MUTEX_INITIALIZER;
static z_owned_publisher_t pub;
static int have_pub;
static uint32_t pongs_failed;
static int answer;

static int send_sample(uint8_t* buf, uint32_t len, uint64_t send_ns, uint32_t seq) {
    t1_hdr_put(buf, send_ns, seq);
    z_owned_bytes_t payload;
    if (z_bytes_copy_from_buf(&payload, buf, len) < 0) {
        return -1;
    }
    return z_publisher_put(z_loan(pub), z_move(payload), NULL) >= 0 ? 0 : -1;
}

static uint8_t* rxbuf; // callback runs on the read task only (one subscriber)
static uint8_t* pongbuf;

static void on_sample(z_loaned_sample_t* sample, void* arg) {
    (void)arg;
    z_bytes_reader_t reader = z_bytes_get_reader(z_sample_payload(sample));
    uint8_t head[T1_HDR];
    if (z_bytes_reader_read(&reader, head, T1_HDR) != T1_HDR) {
        return;
    }
    uint64_t send_ns;
    uint32_t seq;
    t1_hdr_get(head, &send_ns, &seq);
    uint64_t now = t1_now_ns();
    pthread_mutex_lock(&score_mutex);
    int first = t1_record(&sc, seq, send_ns, now);
    pthread_mutex_unlock(&score_mutex);
    if (first && answer && send_sample(pongbuf, T1_HDR, send_ns, seq) != 0) {
        pongs_failed++;
    }
}

int main(int argc, char** argv) {
    if (t1_parse(argc, argv, &opt) != 0) {
        fprintf(stderr, "usage: zenohpico_node ping|echo|pub|sub [-d s] [-m be|rel] [-s B] [-r Hz] [-Q s] [-I iface] [-q stop] [-l file]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    uint64_t start = t1_now_ns();
    rxbuf = calloc(1, T1_HDR);
    pongbuf = calloc(1, T1_HDR);
    uint8_t* txbuf = calloc(1, opt.size);
    answer = opt.role == T1_ECHO;

    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "peer");
    if (!opt.reliable) {
        char listen[128];
        snprintf(listen, sizeof(listen), "udp/224.0.0.225:7447#iface=%s", opt.iface != NULL ? opt.iface : "wlan7");
        zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, listen);
    } else {
        const char* l = getenv("T1_ZENOH_LISTEN");
        const char* c = getenv("T1_ZENOH_CONNECT");
        if (l != NULL) {
            zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, l);
        }
        if (c != NULL) {
            zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, c);
        }
    }
    z_owned_session_t session;
    if (z_open(&session, z_move(config), NULL) < 0) {
        printf("RESULT fw=zenohpico role=%d error=open_failed matched=0\n", (int)opt.role);
        return 1;
    }
    zp_start_read_task(z_loan_mut(session), NULL);
    zp_start_lease_task(z_loan_mut(session), NULL);

    z_view_keyexpr_t wke, rke;
    const char* wname = NULL;
    const char* rname = NULL;
    switch (opt.role) {
    case T1_PING: wname = "t1/ping"; rname = "t1/pong"; break;
    case T1_ECHO: wname = "t1/pong"; rname = "t1/ping"; break;
    case T1_PUB: wname = "t1/ping"; break;
    case T1_SUB: rname = "t1/ping"; break;
    }
    z_owned_subscriber_t sub;
    int have_sub = 0;
    if (rname != NULL) {
        z_view_keyexpr_from_str(&rke, rname);
        z_owned_closure_sample_t cb;
        z_closure(&cb, on_sample, NULL, NULL);
        if (z_declare_subscriber(z_loan(session), &sub, z_loan(rke), z_move(cb), NULL) < 0) {
            printf("RESULT fw=zenohpico role=%d error=declare_failed matched=0\n", (int)opt.role);
            return 1;
        }
        have_sub = 1;
    }
    if (wname != NULL) {
        z_view_keyexpr_from_str(&wke, wname);
        z_publisher_options_t po;
        z_publisher_options_default(&po);
        po.congestion_control = opt.reliable ? Z_CONGESTION_CONTROL_BLOCK : Z_CONGESTION_CONTROL_DROP;
#ifdef Z_FEATURE_UNSTABLE_API
        po.reliability = opt.reliable ? Z_RELIABILITY_RELIABLE : Z_RELIABILITY_BEST_EFFORT;
#endif
        if (z_declare_publisher(z_loan(session), &pub, z_loan(wke), &po) < 0) {
            printf("RESULT fw=zenohpico role=%d error=declare_failed matched=0\n", (int)opt.role);
            return 1;
        }
        have_pub = 1;
    }
    (void)have_pub;

    uint32_t published = 0;
    uint32_t failed = 0;
    if (opt.role == T1_PING || opt.role == T1_PUB) {
        // no matched-endpoint API without listeners: settle 3 s as the repository's zenoh-pico harness does
        t1_sleep_until(t1_now_ns() + 3000000000ULL);
        if (opt.role == T1_PING) {
            sc.warmup = (uint32_t)opt.rate_hz;
            uint64_t period = (uint64_t)(1e9 / opt.rate_hz);
            uint64_t end = t1_now_ns() + (uint64_t)(opt.seconds * 1e9);
            for (uint64_t next = t1_now_ns(); next < end && published < T1_MAX_SEQ; next += period) {
                t1_sleep_until(next);
                if (send_sample(txbuf, opt.size, t1_now_ns(), published) == 0) {
                    published++;
                } else {
                    failed++;
                }
            }
            t1_sleep_until(t1_now_ns() + (uint64_t)(T1_DRAIN_S * 1e9));
        } else {
            uint32_t n = (uint32_t)(opt.seconds * opt.rate_hz);
            uint64_t period = (uint64_t)(1e9 / opt.rate_hz);
            uint64_t next = t1_now_ns();
            uint64_t pub_t0 = next;
            for (uint32_t i = 0; i < n; i++, next += period) {
                t1_sleep_until(next);
                if (send_sample(txbuf, opt.size, t1_now_ns(), i) == 0) {
                    published++;
                } else {
                    failed++;
                }
            }
            t1_sleep_until(pub_t0 + (uint64_t)((opt.seconds + opt.drain_s) * 1e9));
        }
    } else {
        sc.warmup = opt.role == T1_ECHO ? (uint32_t)opt.rate_hz : 0;
        uint64_t end = start + (uint64_t)(opt.seconds * 1e9);
        while (t1_now_ns() < end && !t1_stop_asked(&opt)) {
            t1_sleep_until(t1_now_ns() + 10000000ULL);
        }
        failed = pongs_failed;
    }
    pthread_mutex_lock(&score_mutex);
    t1_result("zenohpico", &opt, &sc, 1, published, failed, start, t1_now_ns(), "matched_basis=settle3s");
    pthread_mutex_unlock(&score_mutex);
    (void)have_sub;
    _exit(0);
}
