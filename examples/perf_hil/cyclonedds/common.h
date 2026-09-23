/*
 * Shared helper for every CycloneDDS HIL scenario (rmw_tickle/COMPARISON.MD) - actively waits for
 * a real match instead of a blind sleep.
 *
 * THE ACTUAL ROOT CAUSE (2026-09-20, found by diffing against the real upstream
 * eclipse-cyclonedds/cyclonedds examples/throughput/publisher.c's own wait_for_reader(), at the
 * user's own explicit suggestion - "우리가 DDS 자체를 잘 못 이해하고 사용하는 것일 수도 있어. ...
 * 공식 예제들과 비교해 보았을 때 특별한 문제가 없는지 한 번 확인해보자" - and again, after a first
 * fix attempt still failed - "공식 예제를 최대한 따르는 것이 안전할 것 같아... API를 기준으로 만들기
 * 보다는 공식 예제를 조금씩 변형해 가는 식으로 작업 하면 좋을 것 같아": this earlier version polled
 * dds_get_*_matched_status() in a plain nanosleep() loop - close to, but not exactly, the real
 * upstream pattern (dds_set_status_mask() then dds_create_waitset()+dds_waitset_attach()+
 * dds_waitset_wait(), a single blocking wait rather than a sleep/poll loop). Bisected directly,
 * not assumed: a real diagnostic reader (waitset-based) matched a real production writer fine,
 * and a real diagnostic writer (waitset-based) matched a real production reader fine - but the
 * production *writer*'s own nanosleep-poll version of this exact wait never saw the match, even
 * though the peer reader genuinely had matched by every other measure. The nanosleep-poll
 * approach itself is the defect, not a missing status-mask bit (tried and ruled out first) or a
 * CycloneDDS version issue (ruled out separately - 11.0.1 had the identical symptom) or a real
 * network/discovery problem (ruled out via real packet capture). Rewritten here to match the
 * official example's own waitset pattern exactly instead of a plausible-looking reimplementation.
 */
#ifndef PERF_HIL_CYCLONEDDS_COMMON_H
#define PERF_HIL_CYCLONEDDS_COMMON_H

#include <stdbool.h>
#include <stdio.h>

#include <dds/dds.h>

// Matches eclipse-cyclonedds/cyclonedds's own examples/throughput/publisher.c wait_for_reader()
// exactly: enable the matched-status, attach it to a fresh waitset, block once on that waitset
// (not a sleep/poll loop) up to timeout_s. Returns true once matched, false on timeout - callers
// should treat a false return as a real failure (log and exit), not silently proceed to measure
// against an unmatched peer. The waitset itself is deleted before returning - it isn't needed
// once this one-time wait is over, unlike a long-lived data-availability waitset.
// Preserves/restores the writer's existing status mask the same way wait_for_reader_match does
// below (see its own doc comment for the real bug this avoids) - no known call site currently
// relies on a writer's own pre-existing mask, but getting this one out of sync with its reader
// counterpart is exactly how the reader-side bug went unnoticed for as long as it did.
static inline bool wait_for_writer_match(dds_entity_t participant, dds_entity_t writer, double timeout_s) {
    uint32_t original_mask = 0;
    dds_return_t rc = dds_get_status_mask(writer, &original_mask);
    if (rc != DDS_RETCODE_OK) {
        fprintf(stderr, "dds_get_status_mask (writer): %s\n", dds_strretcode(-rc));
        return false;
    }
    rc = dds_set_status_mask(writer, original_mask | DDS_PUBLICATION_MATCHED_STATUS);
    if (rc != DDS_RETCODE_OK) {
        fprintf(stderr, "dds_set_status_mask (writer): %s\n", dds_strretcode(-rc));
        return false;
    }
    dds_entity_t waitset = dds_create_waitset(participant);
    dds_waitset_attach(waitset, writer, 0);
    rc = dds_waitset_wait(waitset, NULL, 0, (dds_duration_t)(timeout_s * 1e9));
    dds_delete(waitset);
    dds_set_status_mask(writer, original_mask);
    if (rc <= 0) {
        return false;
    }
    dds_publication_matched_status_t status;
    dds_get_publication_matched_status(writer, &status);
    return status.current_count > 0;
}

// dds_set_status_mask() REPLACES the entire mask, it doesn't OR a bit in (2026-09-20, real bug
// found the hard way): every latency-scenario caller sets DDS_DATA_AVAILABLE_STATUS on its reader
// *before* this call so its own long-lived receive waitset will wake on real data later - calling
// dds_set_status_mask(reader, DDS_SUBSCRIPTION_MATCHED_STATUS) here used to clobber that outright
// and never restored it, so after a successful match-wait the reader's own waitset could still
// wait indefinitely for DATA_AVAILABLE_STATUS *even after real data actually arrived*, silently
// invisibly at that call site. It's why best_effort_latency/reliable_latency showed a clean match
// (this function correctly returning true) immediately followed by permanent 100% RTT loss on the
// rig - the reader had a real match and was really receiving pongs at the RTPS layer, but its own
// mask no longer included the one bit its receive-side waitset was actually blocked on. Preserving
// and restoring the mask here (OR the match bit in for the wait, then put back exactly what the
// caller had) is what the official examples don't need to worry about since roundtrip's ping.c/
// pong.c never call this function at all (see best_effort_throughput/client.c's own doc comment).
static inline bool wait_for_reader_match(dds_entity_t participant, dds_entity_t reader, double timeout_s) {
    uint32_t original_mask = 0;
    dds_return_t rc = dds_get_status_mask(reader, &original_mask);
    if (rc != DDS_RETCODE_OK) {
        fprintf(stderr, "dds_get_status_mask (reader): %s\n", dds_strretcode(-rc));
        return false;
    }
    rc = dds_set_status_mask(reader, original_mask | DDS_SUBSCRIPTION_MATCHED_STATUS);
    if (rc != DDS_RETCODE_OK) {
        fprintf(stderr, "dds_set_status_mask (reader): %s\n", dds_strretcode(-rc));
        return false;
    }
    dds_entity_t waitset = dds_create_waitset(participant);
    dds_waitset_attach(waitset, reader, 0);
    rc = dds_waitset_wait(waitset, NULL, 0, (dds_duration_t)(timeout_s * 1e9));
    dds_delete(waitset);
    dds_set_status_mask(reader, original_mask);
    if (rc <= 0) {
        return false;
    }
    dds_subscription_matched_status_t status;
    dds_get_subscription_matched_status(reader, &status);
    return status.current_count > 0;
}

#endif
