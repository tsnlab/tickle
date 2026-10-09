/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g15_zerocopy_e2e.sh's publisher: publishes COUNT best-effort samples of BODY bytes through a topic that offers an
// in-place encode, and prints what the context counted beside what the kernel's interface counted, with the number of
// in-place encodes as the witness that the zero-copy path is the one that sent them.
//
// The perf_hil benches cannot be the witness here: none of p1-p4 reaches publish_zerocopy()'s single-datagram send.
// try_publish_zerocopy() takes the path only for a body that is 4-aligned and too large for a second DATA to share
// its datagram - 1429 to 1448 bytes at the 1472-byte datagram - and p3, the nearest, is 1424. So the body size is
// chosen here, and the in-place encode counts its own calls: that count is the zero-copy path's, since
// try_publish_zerocopy() is the only caller of data_encode_inplace on the send side.
//
// usage: g15_zerocopy_e2e <interface> <body bytes> <count>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#define BODY_MAX 4096
#define PATH_MAX_BYTES 256
#define FILL_BYTE 0x5A

static uint32_t g_body_len;
static uint64_t g_inplace_encodes;

struct body {
    uint8_t bytes[BODY_MAX];
};

static int32_t body_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)g_body_len;
}

static int32_t body_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < g_body_len) {
        return -1;
    }
    memcpy(payload, data, g_body_len);
    return (int32_t)g_body_len;
}

static int32_t body_encode_inplace(struct tt_Data* data, const uint8_t** payload_out) {
    g_inplace_encodes++;
    *payload_out = (const uint8_t*)data;
    return (int32_t)g_body_len;
}

static void body_free(struct tt_Data* data) {
    (void)data;
}

static uint64_t tx_packets(const char* iface) {
    char path[PATH_MAX_BYTES];
    (void)snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_packets", iface);
    FILE* file = fopen(path, "r");
    unsigned long long value = 0;
    if (file == NULL || fscanf(file, "%llu", &value) != 1) {
        value = 0;
    }
    if (file != NULL) {
        (void)fclose(file);
    }
    return (uint64_t)value;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <interface> <body bytes> <count>\n", argv[0]);
        return 2;
    }
    const char* iface = argv[1];
    g_body_len = (uint32_t)strtoul(argv[2], NULL, 10);
    const uint32_t count = (uint32_t)strtoul(argv[3], NULL, 10);
    if (g_body_len == 0 || g_body_len > BODY_MAX) {
        fprintf(stderr, "body must be 1..%d bytes\n", BODY_MAX);
        return 2;
    }
    _tt_CONFIG.broadcast = "192.168.10.255";

    static struct tt_Topic topic = {
        .name = "g15_zerocopy",
        .data_size = sizeof(struct body),
        .data_encode_size = body_encode_size,
        .data_encode = body_encode,
        .data_encode_inplace = body_encode_inplace,
        .data_free = body_free,
    };
    static struct tt_Context node;
    static struct tt_Publisher pub;
    static struct body sample;
    memset(&sample, FILL_BYTE, sizeof(sample));
    // The sample's own size, not the buffer's: a topic larger than this build's largest sample is refused at create.
    topic.data_size = g_body_len;
    if (tt_Context_create(&node) != tt_RET_OK || tt_Context_create_publisher(&node, &pub, &topic, "g15") != tt_RET_OK) {
        fprintf(stderr, "cannot create the context or its publisher\n");
        return 1;
    }
    (void)tt_Context_poll(&node, 0); // the startup announce goes out before the window opens

    const uint64_t packets_before = tx_packets(iface);
    const uint64_t datagrams_before = node.tx_datagrams;
    const uint64_t udp_before = node.tx_datagrams_by_transport[tt_TRANSPORT_UDP];
    uint32_t published = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (tt_Publisher_publish(&pub, (struct tt_Data*)&sample) == tt_RET_OK) {
            published++;
        }
    }
    const uint64_t packets = tx_packets(iface) - packets_before;
    printf("G15: body=%u count=%u published=%u inplace_encodes=%llu tx_datagrams=%llu tx_udp=%llu tx_packets=%llu\n",
           (unsigned)g_body_len, (unsigned)count, (unsigned)published, (unsigned long long)g_inplace_encodes,
           (unsigned long long)(node.tx_datagrams - datagrams_before),
           (unsigned long long)(node.tx_datagrams_by_transport[tt_TRANSPORT_UDP] - udp_before),
           (unsigned long long)packets);
    tt_Context_destroy(&node);
    return 0;
}
