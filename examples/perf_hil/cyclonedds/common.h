/*
 * Shared helper for every CycloneDDS HIL scenario (rmw_tickle/comparison.md) - actively waits for
 * a real match instead of a blind sleep.
 *
 * Root-caused, not guessed: CycloneDDS's own default SPDP (participant discovery) multicast
 * announce interval is 30s (confirmed via CYCLONEDDS_URI Tracing=fine - "Domain/Discovery/
 * SPDPInterval/#text: 30 s"). Each participant also announces once immediately at startup, so a
 * short fixed sleep (e.g. 2s) *usually* catches that initial announce and works - but if either
 * side starts its listener even slightly late relative to the other's one-shot startup announce,
 * the next opportunity is the full 30s later, not "a bit more than 2s." This is exactly what made
 * earlier ad-hoc test runs in this same session flip between working and completely silent with
 * no code change at all - not flakiness, a real timing race with no margin.
 */
#ifndef PERF_HIL_CYCLONEDDS_COMMON_H
#define PERF_HIL_CYCLONEDDS_COMMON_H

#include <dds/dds.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

// Polls the given writer/reader's own matched-count status until non-zero or timeout_s elapses.
// Returns true once matched, false on timeout - callers should treat a false return as a real
// failure (log and exit), not silently proceed to measure against an unmatched peer.
static inline bool wait_for_writer_match(dds_entity_t writer, double timeout_s) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        dds_publication_matched_status_t status;
        // Not gated on dds_get_publication_matched_status()'s own return code - a real, bisected
        // finding (not assumed): on this install it does not reliably return DDS_RETCODE_OK even
        // once current_count has genuinely gone non-zero (most likely because nothing here ever
        // calls dds_set_status_mask() to enable DDS_PUBLICATION_MATCHED_STATUS tracking on this
        // writer) - current_count itself is populated correctly regardless, confirmed by an
        // instrumented debug build showing a real, timely match that this exact check was
        // silently discarding.
        dds_get_publication_matched_status(writer, &status);
        if (status.current_count > 0) {
            return true;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= timeout_s) {
            return false;
        }
        struct timespec poll_interval = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000}; // 50ms
        nanosleep(&poll_interval, NULL);
    }
}

static inline bool wait_for_reader_match(dds_entity_t reader, double timeout_s) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        dds_subscription_matched_status_t status;
        // See wait_for_writer_match()'s own identical note - same fix, same reason.
        dds_get_subscription_matched_status(reader, &status);
        if (status.current_count > 0) {
            return true;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= timeout_s) {
            return false;
        }
        struct timespec poll_interval = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
        nanosleep(&poll_interval, NULL);
    }
}

#endif
