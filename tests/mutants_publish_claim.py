#!/usr/bin/env python3
"""Mutant sweep for the claimed-slot publish (tt_Publisher_claim(), tests/test_publish_claim.c).

Each entry removes or bends exactly one condition or step of the claim, its publish or its release and names the test that must
go red for it. A mutant that survives is a condition no test decides - either the test is missing or the condition
is not doing anything, and both are findings.

Run from the repository root. Restores every file it touches, whatever happens.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_publish_claim"

MUTANTS = [
    (
        "peer_segment() keeping a claimed mapping",
        "src/tickle.c",
        "    if (entry != NULL && entry->claims != 0) {\n        const bool same_owner =",
        "    if (false) {\n        const bool same_owner =",
        "test_a_claimed_mapping_stays_mapped",
    ),
    (
        "the dead-reader rule keeping a claimed mapping",
        "src/tickle.c",
        "        if (now - entry->last_progress_ns >= tt_SEGMENT_DEAD_READER_NS && entry->claims == 0) {",
        "        if (now - entry->last_progress_ns >= tt_SEGMENT_DEAD_READER_NS) {",
        "test_a_claimed_mapping_outlives_the_dead_reader_rule",
    ),
    (
        "counting the claim on its mapping",
        "src/tickle.c",
        "    segment_claims_note(node, dest.context_id, 1);\n",
        "",
        "test_a_claimed_mapping_stays_mapped",
    ),
    (
        "the claim held at once",
        "src/tickle.c",
        "    __atomic_store_n(&slot->sequence, claimed + 2U, __ATOMIC_RELEASE);\n    segment_claims_note(",
        "    segment_claims_note(",
        "test_several_claims_and_publishes_meanwhile",
    ),
    (
        "the claim limit",
        "src/tickle.c",
        "    if (claim == NULL) {\n        return tt_RET_ILLEGAL_STATUS;\n    }\n    struct tt_Context* node = pub->node;\n"
        "    if (!keep_all_writable(pub)) {\n        // As publisher_publish_locked() refuses",
        "    if (claim == NULL) {\n        claim = &pub->claims[0];\n    }\n    struct tt_Context* node = pub->node;\n"
        "    if (!keep_all_writable(pub)) {\n        // As publisher_publish_locked() refuses",
        "test_several_claims_and_publishes_meanwhile",
    ),
    (
        "a payload matched to its claim",
        "src/tickle.c",
        "        if (pub->claims[k].slot != NULL && claim_payload(&pub->claims[k]) == payload) {",
        "        if (pub->claims[k].slot != NULL) {",
        "test_several_claims_and_publishes_meanwhile",
    ),
    (
        "KEEP_ALL's refusal at the claim",
        "src/tickle.c",
        "    if (!keep_all_writable(pub)) {\n        // As publisher_publish_locked() refuses",
        "    if (false) {\n        // As publisher_publish_locked() refuses",
        "test_full_ring_and_keep_all",
    ),
    (
        "KEEP_ALL's refusal at the publish",
        "src/tickle.c",
        "    if (!keep_all_writable(pub)) {\n        // Ordinary publishes since the claim",
        "    if (false) {\n        // Ordinary publishes since the claim",
        "test_full_ring_and_keep_all",
    ),
    (
        "the reader's feature asked",
        "src/tickle.c",
        "(segment->features & tt_SEGMENT_FEATURE_HELD_REF) == 0 ||",
        "false ||",
        "test_what_cannot_be_claimed",
    ),
    (
        "the destination rule at the claim",
        "src/tickle.c",
        "    if (capacity > (uint32_t)tt_MAX_SAMPLE_LENGTH || !encode_in_slot_destination(pub, node->tx_tail, &dest)) {",
        # The rule not asked: the destination taken as the first peer, whatever the publish's shape.
        "    dest = (struct tx_destination){pub->peers[0].ip, pub->peers[0].port, pub->peers[0].context_id};\n"
        "    if (capacity > (uint32_t)tt_MAX_SAMPLE_LENGTH) {",
        "test_what_cannot_be_claimed",
    ),
    (
        "the one-datagram bound at the claim",
        "src/tickle.c",
        "        return tt_RET_UNSUPPORTED; // it would fragment: one record, one seq_no, or the ordinary path",
        "        (void)0; // it would fragment: one record, one seq_no, or the ordinary path",
        "test_what_cannot_be_claimed",
    ),
    (
        "the destination asked again at the publish",
        "src/tickle.c",
        "    if (!encode_in_slot_destination(pub, node->tx_tail, &dest) || dest.context_id != claim->context_id ||",
        "    dest = (struct tx_destination){pub->peers[0].ip, pub->peers[0].port, pub->peers[0].context_id};\n"
        "    if (dest.context_id != claim->context_id ||",
        "test_a_changed_destination_is_published_by_copy",
    ),
    (
        "the copy's slot given back",
        "src/tickle.c",
        "    pub->topic = topic;\n    claim_release(pub, claim, false);\n    node->segment_claims_copied++;",
        "    pub->topic = topic;\n    claim_release(pub, claim, true);\n    node->segment_claims_copied++;",
        "test_a_changed_destination_is_published_by_copy",
    ),
    (
        "the length bounded by the capacity",
        "src/tickle.c",
        "    if (length > claim->length) {\n        // Spent, as every publish",
        "    if (false) {\n        // Spent, as every publish",
        "test_a_claim_publishes_its_length_not_its_capacity",
    ),
    (
        "the padding zeroed",
        "src/tickle.c",
        "    memset(record + raw_len, 0, record_len - raw_len);\n    if (!check_and_cache_sample(node, pub, "
        "submessage_header, raw_len, FRAG_WHOLE_DATA_LIMIT)) {\n        claim_release(pub, claim, false);",
        "    if (!check_and_cache_sample(node, pub, "
        "submessage_header, raw_len, FRAG_WHOLE_DATA_LIMIT)) {\n        claim_release(pub, claim, false);",
        "test_a_claimed_publish_writes_what_the_encoder_writes",
    ),
    (
        "the reliable cache's copy",
        "src/tickle.c",
        "    if (!check_and_cache_sample(node, pub, submessage_header, raw_len, FRAG_WHOLE_DATA_LIMIT)) {\n"
        "        claim_release(pub, claim, false);",
        "    if (false) {\n        claim_release(pub, claim, false);",
        "test_reliable_claims_cache_what_the_encoder_caches",
    ),
    (
        "the single header written",
        "src/tickle.c",
        "    _tt_memcpy(record, &single, sizeof(single));\n    // The reference,",
        "    // The reference,",
        "test_a_claimed_publish_writes_what_the_encoder_writes",
    ),
    (
        "the reference's length",
        "src/tickle.c",
        "        const struct tt_SegmentRef desc = {claim->index, record_len};",
        "        const struct tt_SegmentRef desc = {claim->index, claim->length};",
        "test_a_claim_publishes_its_length_not_its_capacity",
    ),
    (
        "the reference's slot",
        "src/tickle.c",
        "        const struct tt_SegmentRef desc = {claim->index, record_len};",
        "        const struct tt_SegmentRef desc = {ref_index, record_len};",
        "test_a_claimed_publish_writes_what_the_encoder_writes",
    ),
    (
        "no room for the reference: slot given back",
        "src/tickle.c",
        "        node->segment_full_dropped++;\n        claim_release(pub, claim, false);",
        "        node->segment_full_dropped++;\n        claim_release(pub, claim, true);",
        "test_no_room_for_the_reference_drops_as_a_full_ring",
    ),
    (
        "seq_no advanced",
        "src/tickle.c",
        "#endif\n    pub->seq_no += 1;\n    idle_heartbeat_note_publish(pub, now_ns);\n    maybe_solicit_ack_at_watermark(pub);\n"
        "    return tt_RET_OK;\n}\n\nstatic tt_ret_t publisher_abandon_claim_locked(",
        "#endif\n    idle_heartbeat_note_publish(pub, now_ns);\n    maybe_solicit_ack_at_watermark(pub);\n"
        "    return tt_RET_OK;\n}\n\nstatic tt_ret_t publisher_abandon_claim_locked(",
        "test_several_claims_and_publishes_meanwhile",
    ),
    (
        "the doorbell after the publish",
        "src/tickle.c",
        "        segment_note_written(node, dest.context_id, segment, dest.ip, dest.port, true);\n    }",
        "    }",
        "test_a_claimed_publish_writes_what_the_encoder_writes",
    ),
    (
        "an abandoned slot given back",
        "src/tickle.c",
        "    if (!referenced) {\n        segment_unhold(claim->segment, claim->index);\n    }",
        "",
        "test_abandon_and_destroy_give_the_slot_back",
    ),
    (
        "destroy abandoning the claims",
        "src/tickle.c",
        "    // A claim left outstanding would stay held for good (tt_Publisher_claim()).\n"
        "    publisher_abandon_all_claims_locked(pub);\n",
        "",
        "test_abandon_and_destroy_give_the_slot_back",
    ),
    (
        "context teardown abandoning the claims",
        "src/tickle.c",
        "            publisher_abandon_all_claims_locked((struct tt_Publisher*)endpoint);\n",
        "",
        "test_context_destroy_resolves_an_outstanding_claim",
    ),
    (
        "the drain counting a reference read",
        "src/tickle.c",
        "                taken += (unsigned)stall >> 1U;\n",
        "",
        "test_a_sample_read_by_reference_ends_the_poll",
    ),
    (
        "a reference read saying so (the 2026-10-20 stall)",
        "src/tickle.c",
        "lend_finish_release(node) ? HEAD_STALL_READ_LAST : HEAD_STALL_READ;",
        "lend_finish_release(node) ? HEAD_STALL_STOP : HEAD_STALL_PASSED;",
        "test_a_sample_read_by_reference_ends_the_poll",
    ),
]

def build_and_run(control=None):
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True, cwd=ROOT / "platform/linux",
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    if unchanged(control, BIN):
        return NOT_COMPILED, ""
    run = subprocess.run([str(BIN)], capture_output=True, text=True)
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    backup = tempfile.mkdtemp(prefix="claim_mutants_")
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
        control = code_digest(BIN)

        only = os.environ.get("MUTANTS", "")
        for name, rel, old, new, test in MUTANTS:
            if only and only not in name:
                continue
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR]    {name:40s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            assert path.read_text().count(old) == 0, "substitution did not apply"
            verdict, output = build_and_run(control)
            restore()
            ok = verdict == "FAIL"
            failures += 0 if ok else 1
            print(f"[{label(verdict, ok):8s}] {name:40s} -> {verdict} (wanted FAIL, guard: {test})")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)

    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
