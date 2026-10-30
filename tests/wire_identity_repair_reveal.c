/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The capture tests/wire_identity_repair_reveal.sh compares (tt_LinkOptions.repair_reveal, docs/DESIGN.md "Repair
// reveal"): every datagram either context of test_large_sample.c's two-context sim hands its HAL, with the mock clock's
// time and the address it went to, in two lossy scenarios on a reader with WIFI's repair_timing, one file each:
// - large: four 1 MiB RELIABLE samples at 10% loss over a 10 ms path each way;
// - small: 200 RELIABLE samples of 1000 bytes, one every 2 ms, at 10% loss over the same path.
// Built with -DTICKLE_C= a tree's tickle.c and that tree's headers. With -DREVEAL_MODE=n the reader's
// tt_Context.repair_reveal is set to n, where the tree has the field; without it the context's is left as the parent
// left everything: no reveal.

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

// Steps the sim until `until`, leaving the clock there.
static void run_until(uint64_t until) {
    while (sim_next_due() <= until && sim_step()) {
    }
    if (test_mock_now < until) {
        test_mock_now = until;
    }
}

static void wire_scenario(const char* path, bool large) {
    wire_out = fopen(path, "wb");
    if (wire_out == NULL) {
        perror(path);
        exit(2);
    }
    init_pair_with(1000, RELIABLE_KEEP_LAST, PAIR_UNICAST, NULL);
    sim_start(0);
    receiver.repair_timing = true; // WIFI's
#if defined(REVEAL_MODE) && defined(tt_REPAIR_REVEAL_OFF)
    receiver.repair_reveal = REVEAL_MODE;
#endif
    test_mock_send_hook = wire_capture;
    (void)sim_publish(1, tt_SECOND);
    sim_settle(1, tt_SECOND);
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    if (proxy != NULL) {
        proxy->keep_all = tt_WRITER_KEEP_ALL_NO;
    }
    sim_reverse_delay = 10 * tt_MILLISECOND;
    sim_unicast_delay = 10 * tt_MILLISECOND;
    sim_loss_per_mille = 100;
    test_mock_nonblocking_room = -1;
    if (large) {
        sample_len = ONE_MB;
        for (int i = 0; i < 4; i++) {
            (void)sim_publish((uint8_t)(i + 2), tt_SECOND);
            sim_settle(i + 2, 2 * tt_SECOND);
        }
    } else {
        uint64_t start = test_mock_now;
        for (int i = 0; i < 200; i++) {
            (void)sim_publish((uint8_t)(2 + i), tt_SECOND);
            run_until(start + ((uint64_t)(i + 1) * 2 * tt_MILLISECOND));
        }
    }
    sim_loss_per_mille = 0;
    uint64_t quiet_by = test_mock_now + (2 * tt_SECOND); // the last repairs, acknowledgements and chases
    while (test_mock_now < quiet_by && sim_step()) {
    }
    printf("wire_identity_repair_reveal: %s - %d delivered, writer %llu datagrams, reader %llu, %u resent\n", path,
           delivered_count, (unsigned long long)sim_sent[SENDER_ID], (unsigned long long)sim_sent[RECEIVER_ID],
           pub.retransmitted);
    fclose(wire_out);
    teardown_pair();
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s LARGE.bin SMALL.bin\n", argv[0]);
        return 2;
    }
    wire_scenario(argv[1], true);
    wire_scenario(argv[2], false);
    allocator_reset(&sender_alloc);
    allocator_reset(&receiver_alloc);
    free(sim_queue);
    return 0;
}
