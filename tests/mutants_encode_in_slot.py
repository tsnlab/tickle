#!/usr/bin/env python3
"""Mutant sweep for SHM_PLAN 6e(b), encode into the slot (try_publish_into_slot(), tests/test_encode_in_slot.c).

Each entry removes or bends exactly one condition or step of the one-copy publish path and names the test that must
go red for it. A mutant that survives is a condition no test decides - either the test is missing or the condition
is not doing anything, and both are findings.

Run from the repository root. Restores every file it touches, whatever happens.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

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
        "    if (sizeof(struct tt_Header) + record_len > FRAG_WHOLE_DATA_LIMIT) {",
        "    if (false) {",
        "test_what_stays_on_the_staging_path",
    ),
    (
        "the one-datagram bound, one byte short",
        "src/tickle.c",
        "    if (sizeof(struct tt_Header) + record_len > FRAG_WHOLE_DATA_LIMIT) {",
        "    if (sizeof(struct tt_Header) + record_len >= FRAG_WHOLE_DATA_LIMIT) {",
        "test_what_stays_on_the_staging_path",
    ),
    (
        "KEEP_ALL's arena bound asked before the claim",
        "src/tickle.c",
        "    if (keep_all_refused_record_bytes(pub, node, &probe, raw_len, FRAG_WHOLE_DATA_LIMIT) != 0) {",
        "    if (false) {",
        "test_keep_all_refusal_is_unchanged",
    ),
    (
        "declining a record the slot cannot hold",
        "src/tickle.c",
        "    if (segment == NULL || record_len > segment->slot_bytes) {",
        "    if (segment == NULL) {",
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
        "    memset(record + raw_len, 0, record_len - raw_len);",
        "    // mutant: padding left as the slot had it",
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
        "    _tt_memcpy(record, &single, sizeof(single));\n    segment_publish(",
        "    (void)single;\n    segment_publish(",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "flush_tx()'s datagram statistics",
        "src/tickle.c",
        "    g_rstats.datagrams++; // flush_tx()'s accounting",
        "    // mutant: not counted",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "the counted and rung record",
        "src/tickle.c",
        "    segment_note_written(node, dest.context_id, segment, dest.ip, dest.port, true);",
        "    // mutant: neither counted nor rung",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "the doorbell rung for a record written in place",
        "src/tickle.c",
        "    segment_note_written(node, dest.context_id, segment, dest.ip, dest.port, true);",
        "    segment_note_written(node, dest.context_id, segment, dest.ip, dest.port, false);",
        "test_best_effort_slots_match_the_two_copy_path",
    ),
    (
        "the watermark solicitation after a one-copy publish",
        "src/tickle.c",
        "    pub->seq_no += 1;\n    maybe_solicit_ack_at_watermark(pub);",
        "    pub->seq_no += 1;",
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


def build_and_run():
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True, cwd=ROOT / "platform/linux",
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    run = subprocess.run([str(BIN)], capture_output=True, text=True)
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    backup = tempfile.mkdtemp(prefix="slot_mutants_")
    touched = {m[1] for m in MUTANTS}
    for rel in touched:
        shutil.copy2(ROOT / rel, pathlib.Path(backup) / rel.replace("/", "_"))

    def restore():
        for rel in touched:
            # copy() and touch, not copy2(): a restored file must look newer than objects built from the mutant.
            shutil.copy(pathlib.Path(backup) / rel.replace("/", "_"), ROOT / rel)
            (ROOT / rel).touch()

    failures = 0
    try:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                      -> {verdict}")
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1

        for name, rel, old, new, test in MUTANTS:
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR]    {name:40s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            assert path.read_text().count(old) == 0, "substitution did not apply"
            verdict, output = build_and_run()
            restore()
            ok = verdict == "FAIL"
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name:40s} -> {verdict} (wanted FAIL, guard: {test})")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)

    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
