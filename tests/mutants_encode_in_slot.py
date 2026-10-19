#!/usr/bin/env python3
"""Mutant sweep for SHM_PLAN 6e(b), encode into the slot (try_publish_into_slot(), tests/test_encode_in_slot.c).

Each entry removes or bends exactly one condition or step of the one-copy publish path and names the test that must
go red for it. A mutant that survives is a condition no test decides - either the test is missing or the condition
is not doing anything, and both are findings.

Run from the repository root. MUTANTS=<substring> runs only the mutants whose name contains it (the control always
runs). Refuses to start (exit 2) if any anchor is not in its file exactly once - "the watermark solicitation after a
one-copy publish" matched nothing from the day idle_heartbeat_note_publish() was put before the solicitation, and the
sweep ran on reporting it as one failure among the rest - or if a file it edits is already modified. Every child is
bounded (tests/mutant_guard.py: 2 GB of address space, a deadline, at most 256 KB of its output kept); every file is
restored after each mutant, on an exception and on SIGINT/SIGTERM/SIGHUP, with a fresh mtime so make rebuilds.
"""
import os
import pathlib
import sys

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, killed_by, run_capped, usage_line

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_encode_in_slot"

MUTANTS = [
    # Stage 1 / S1 (2026-10-09): a destination with no segment declined before the slot path's checks, not after.
    (
        "declining in a context that never attached a segment, before the checks",
        "src/tickle.c",
        "    if (node->segment_slot_ceiling == 0) {\n        return false;\n    }\n    // No peers is a broadcast",
        "    // No peers is a broadcast",
        "test_a_peer_on_another_host_costs_the_slot_path_nothing",
    ),
    (
        "declining a batching publisher",
        "src/tickle.c",
        "    if (pub->batch || node->summary_skip_armed || pub->match_heartbeat_pending) {",
        "    if (node->summary_skip_armed || pub->match_heartbeat_pending) {",
        "test_what_stays_on_the_staging_path",
    ),
    (
        "declining when a summary may ride ahead",
        "src/tickle.c",
        "    if (pub->batch || node->summary_skip_armed || pub->match_heartbeat_pending) {",
        "    if (pub->batch || pub->match_heartbeat_pending) {",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "declining when a match Heartbeat is owed",
        "src/tickle.c",
        "    if (pub->batch || node->summary_skip_armed || pub->match_heartbeat_pending) {",
        "    if (pub->batch || node->summary_skip_armed) {",
        "test_what_stays_on_the_staging_path",
    ),
    (
        "declining when a Heartbeat piggybacks",
        "src/tickle.c",
        "        pub->heartbeat_piggyback_count + 1U >= pub->heartbeat_piggyback_every) {",
        "        false) {",
        "test_reliable_records_and_cache_match",
    ),
    (
        "declining for a local Subscriber",
        "src/tickle.c",
        "    if (pub->local_subscriber_count != 0) {\n        return false;\n    }\n#endif\n    // A context that has never",
        "    if (false) {\n        return false;\n    }\n#endif\n    // A context that has never",
        "test_what_stays_on_the_staging_path",
    ),
    (
        "declining more than one destination",
        "src/tickle.c",
        "    if (tx_destinations(peers, peer_count, destinations) != 1) {",
        "    if (tx_destinations(peers, peer_count, destinations) == 0) {",
        "test_what_stays_on_the_staging_path",
    ),
    (
        "declining a size the topic cannot state",
        "src/tickle.c",
        "    if (cdr_len < 0) {\n        return false; // the staging path reports it;",
        "    if (false) {\n        return false; // the staging path reports it;",
        "test_a_failed_encode_publishes_a_harmless_record",
    ),
    (
        "the one-datagram bound, removed",
        "src/tickle.c",
        "    if (sizeof(struct tt_Header) + record_len > FRAG_WHOLE_DATA_LIMIT) {\n        return false;",
        "    if (false) {\n        return false;",
        "test_what_stays_on_the_staging_path",
    ),
    (
        "the one-datagram bound, one byte short",
        "src/tickle.c",
        "    if (sizeof(struct tt_Header) + record_len > FRAG_WHOLE_DATA_LIMIT) {\n        return false;",
        "    if (sizeof(struct tt_Header) + record_len >= FRAG_WHOLE_DATA_LIMIT) {\n        return false;",
        "test_what_stays_on_the_staging_path",
    ),
    (
        "KEEP_ALL's arena bound asked before the claim",
        "src/tickle.c",
        "    if (keep_all_refused_record_bytes(pub, node, &probe, raw_len, FRAG_WHOLE_DATA_LIMIT) != 0) {\n        return false;",
        "    if (false) {\n        return false;",
        "test_keep_all_refusal_is_unchanged",
    ),
    (
        "declining a record the slot cannot hold",
        "src/tickle.c",
        "    if (segment == NULL || record_len > segment->slot_bytes) {\n        return false;",
        "    if (segment == NULL) {\n        return false;",
        "test_what_stays_on_the_staging_path",
    ),
    (
        "an encoder failure detected",
        "src/tickle.c",
        "    if (encoded_len < 0 || !check_and_cache_sample(",
        "    if (!check_and_cache_sample(",
        "test_a_failed_encode_publishes_a_harmless_record",
    ),
    (
        "a claimed slot published on failure",
        "src/tickle.c",
        "        segment_publish(slot, claimed, 0, own_ip, own_port, 1); // the harmless record",
        "        // mutant: the claimed slot left unpublished",
        "test_a_failed_encode_publishes_a_harmless_record",
    ),
    (
        "the failure record being zero-length",
        "src/tickle.c",
        "        segment_publish(slot, claimed, 0, own_ip, own_port, 1); // the harmless record",
        "        segment_publish(slot, claimed, record_len, own_ip, own_port, 1); // mutant",
        "test_a_failed_encode_publishes_a_harmless_record",
    ),
    (
        "the padding zeroed",
        "src/tickle.c",
        "    memset(record + raw_len, 0, record_len - raw_len);\n    if (encoded_len < 0",
        "    // mutant: padding left as the slot had it\n    if (encoded_len < 0",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "the reliable cache's copy",
        "src/tickle.c",
        "    if (encoded_len < 0 || !check_and_cache_sample(node, pub, submessage_header, raw_len, FRAG_WHOLE_DATA_LIMIT)) {",
        "    if (encoded_len < 0) {",
        "test_reliable_records_and_cache_match",
    ),
    (
        "the piggyback cadence counting this publish",
        "src/tickle.c",
        "    (void)piggyback_due(pub, true);",
        "    // mutant: cadence not advanced",
        "test_reliable_records_and_cache_match",
    ),
    (
        "the single header written over the submessage header",
        "src/tickle.c",
        "    _tt_memcpy(record, &single, sizeof(single));\n    segment_publish(slot,",
        "    (void)single;\n    segment_publish(slot,",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "flush_tx()'s datagram statistics",
        "src/tickle.c",
        "    segment_publish(slot, claimed, record_len, own_ip, own_port, 1);\n#ifdef tt_RELIABLE_STATS\n"
        "    g_rstats.datagrams++; // flush_tx()'s accounting",
        "    segment_publish(slot, claimed, record_len, own_ip, own_port, 1);\n#ifdef tt_RELIABLE_STATS\n"
        "    // mutant: not counted",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "the counted and rung record",
        "src/tickle.c",
        "    node->segment_encoded_in_slot++;\n"
        "    segment_note_written(node, dest.context_id, segment, dest.ip, dest.port, true);",
        "    node->segment_encoded_in_slot++;\n    // mutant: neither counted nor rung",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "the doorbell rung for a record written in place",
        "src/tickle.c",
        "    node->segment_encoded_in_slot++;\n"
        "    segment_note_written(node, dest.context_id, segment, dest.ip, dest.port, true);",
        "    node->segment_encoded_in_slot++;\n"
        "    segment_note_written(node, dest.context_id, segment, dest.ip, dest.port, false);",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "the watermark solicitation after a one-copy publish",
        "src/tickle.c",
        "    idle_heartbeat_note_publish(pub, now_ns);\n    maybe_solicit_ack_at_watermark(pub);\n    *result = tt_RET_OK;",
        "    idle_heartbeat_note_publish(pub, now_ns);\n    *result = tt_RET_OK;",
        "test_reliable_records_and_cache_match",
    ),
    (
        "the treatment counter",
        "src/tickle.c",
        "    node->segment_encoded_in_slot++;\n",
        "",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
]


def build_and_run(control=None):
    build = run_capped(["make", "-s", "-j4", str(BIN.relative_to(ROOT / "platform/linux"))], timeout=900,
                       cwd=ROOT / "platform/linux")
    if build.returncode != 0:
        return "BUILD_FAILED", build.output[-2000:]
    if unchanged(control, BIN):
        return NOT_COMPILED, ""
    run = run_capped([str(BIN)], timeout=300)
    if run.timed_out:
        return "HANG", run.output[-1000:]
    if run.returncode is None or run.returncode < 0:
        return "CRASH", run.output[-2000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.output


def failing_lines(output):
    return [line for line in output.splitlines() if "test_encode_in_slot.c:" in line][:3]


def main():
    be_nice()
    check_anchors(ROOT, [(name, rel, old, new) for name, rel, old, new, _test in MUTANTS])
    touched = sorted({m[1] for m in MUTANTS})
    failures = 0
    with SourceGuard(ROOT, touched) as guard:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                      -> {verdict}", flush=True)
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1
        control = code_digest(BIN)

        only = os.environ.get("MUTANTS", "")
        for name, rel, old, new, test in MUTANTS:
            if only and only not in name:
                continue
            (ROOT / rel).write_text(guard.original(rel).replace(old, new, 1))
            try:
                verdict, output = build_and_run(control)
            finally:
                guard.restore()
            ok = verdict in ("FAIL", "CRASH", "HANG")
            failures += 0 if ok else 1
            print(f"[{label(verdict, ok):8s}] {name:40s} -> {verdict} (wanted FAIL, guard: {test})", flush=True)
            if ok:
                print(f"           killed by: {', '.join(killed_by(output, ROOT)) or '(no failing check printed)'}")
            for line in failing_lines(output) if ok else []:
                print(f"           {line}")
            if not ok:
                print(output[-1500:])

    print(usage_line())
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
