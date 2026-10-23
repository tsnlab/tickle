/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The capture tests/wire_identity_resend_order.sh compares (the large-sample resend order, DESIGN.md section 4): every
// datagram either context of test_large_sample.c's two-context sim hands its HAL - the writer's and the reader's - with
// the mock clock's time and the address it went to, in three scenarios, one file each:
// - lossless: five 1 MiB RELIABLE samples and a small one, nothing lost - every byte and time must stay as they were;
// - tails: each sample's last fragment lost once over a 0 ms path - one request, one resend - alike as well;
// - delayed: the same over a 40 ms unicast path - the reader asks again while the repair is on its way, which the rule
//   leaves unanswered: the one scenario that must DIFFER from the parent.
// Built with -DTICKLE_C= a tree's tickle.c and that tree's headers; test_large_sample.c's own tests are not run.

#define main test_large_sample_main // NOLINT(readability-identifier-naming) -- reuse the sim, not its tests
#include "test_large_sample.c"      // NOLINT(bugprone-suspicious-include) -- the sim is that test's
#undef main

static FILE* wire_out;

// One record: time (8 B), the sender's context (1 B), destination ip (4 B), length (4 B), the bytes.
static void wire_capture(const void* buf, size_t len) {
    uint64_t now = test_mock_now;
    uint8_t from = sim_acting;
    uint32_t to = test_mock_send_hook_ip;
    uint32_t length = (uint32_t)len;
    fwrite(&now, sizeof(now), 1, wire_out);
    fwrite(&from, sizeof(from), 1, wire_out);
    fwrite(&to, sizeof(to), 1, wire_out);
    fwrite(&length, sizeof(length), 1, wire_out);
    fwrite(buf, 1, len, wire_out);
    sim_capture(buf, len);
}

static void wire_scenario(const char* path, bool drop_tails, uint64_t delay) {
    wire_out = fopen(path, "wb");
    if (wire_out == NULL) {
        perror(path);
        exit(2);
    }
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    test_mock_send_hook = wire_capture;
    (void)sim_publish(1, tt_SECOND);
    sim_settle(1, tt_SECOND);
    sim_unicast_delay = delay;
    sim_drop_tails = drop_tails;
    sample_len = ONE_MB;
    test_mock_nonblocking_room = -1;
    for (int i = 0; i < 5; i++) {
        (void)sim_publish((uint8_t)(i + 2), tt_SECOND);
        sim_settle(i + 2, 2 * tt_SECOND);
    }
    sample_len = 1000;
    (void)sim_publish(7, tt_SECOND);
    sim_settle(7, tt_SECOND);
    uint64_t quiet_by = test_mock_now + tt_SECOND; // the last acknowledgements and chases
    while (test_mock_now < quiet_by && sim_step()) {
    }
    printf("wire_identity_resend_order: %s - %d delivered, writer %llu datagrams, reader %llu\n", path, delivered_count,
           (unsigned long long)sim_sent[SENDER_ID], (unsigned long long)sim_sent[RECEIVER_ID]);
    fclose(wire_out);
    teardown_pair();
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s LOSSLESS.bin TAILS.bin DELAYED.bin\n", argv[0]);
        return 2;
    }
    wire_scenario(argv[1], false, 0);
    wire_scenario(argv[2], true, 0);
    wire_scenario(argv[3], true, 40 * tt_MILLISECOND);
    allocator_reset(&sender_alloc);
    allocator_reset(&receiver_alloc);
    free(sim_queue);
    return 0;
}
