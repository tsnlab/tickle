#!/usr/bin/env python3
"""Mutant sweep for large-message stage 2, step A (DESIGN.md section 8, "Stage 2", pre-registration L1).

Each entry breaks exactly one thing the stage-2 code relies on and names the test that must go red for it - in
tests/test_large_sample.c unless the entry names another test binary - the mutants the pre-registration names (an off-by-one in the 16-bit offset arithmetic, no
end-of-sample HEARTBEAT, WOULD_BLOCK where a sample can never fit, the send cursor dropped, a torn sample delivered, a
lent buffer reused before its release), and a few more for conditions the tests claim to cover. A mutant that still
passes means either the test does not test what its name says, or the condition decides nothing and should not be in
the code. L1.2's own mutant (small samples sent as large) is in tests/wire_identity_largemsg.sh, which compares
against the parent's bytes.

Run from the repository root. Restores every file it touches, whatever happens. MUTANTS=<substring> runs only the
mutants whose name contains it (the control always runs). A mutant whose binary, debug sections aside, is the control's
is reported DEAD (NOT_COMPILED) and counted as a problem: it edited code this build leaves out, so it can neither die nor
survive - "a lent buffer handed back" sat in the #else of LARGE_DIRECT that way and read as a survivor (2026-10-15).
"""
import pathlib
import shutil
import signal
import subprocess
import sys
import os
import tempfile

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged

ROOT = pathlib.Path(__file__).resolve().parents[1]
TESTS = ROOT / "platform/linux/obj/debug/tests"
DEFAULT_BINARY = "test_large_sample"
SRC = "src/tickle.c"

MUTANTS = [
    (
        "offset: fragment i starts one continuation late",
        "    return index == 0 ? 0 : LARGE_FIRST_PAYLOAD + ((index - 1) * LARGE_CONT_PAYLOAD);",
        "    return index == 0 ? 0 : LARGE_FIRST_PAYLOAD + (index * LARGE_CONT_PAYLOAD);",
        "test_round_trip_at_every_boundary",
    ),
    (
        "offset: one byte off",
        "    return index == 0 ? 0 : LARGE_FIRST_PAYLOAD + ((index - 1) * LARGE_CONT_PAYLOAD);",
        "    return index == 0 ? 0 : LARGE_FIRST_PAYLOAD + 1 + ((index - 1) * LARGE_CONT_PAYLOAD);",
        "test_round_trip_at_every_boundary",
    ),
    (
        "16-bit index sent as 8 bits",
        "        cont->frag_index = (uint16_t)index;",
        "        cont->frag_index = (uint8_t)index;",
        "test_round_trip_at_every_boundary (256 fragments)",
    ),
    (
        "16-bit count sent as 8 bits",
        "        first->frag_count = (uint16_t)count;",
        "        first->frag_count = (uint8_t)count;",
        "test_round_trip_at_every_boundary (256 fragments)",
    ),
    (
        "a fragment's length is not checked against its index",
        "    if (index != last ? fragment->length != full : fragment->length > full) {",
        "    if (fragment->length > LARGE_CONT_PAYLOAD) {",
        "test_an_inconsistent_fragment_is_dropped_and_counted",
    ),
    (
        "no end-of-sample HEARTBEAT",
        "    large_end_heartbeat(node, pub);\n",
        "",
        "test_a_lost_tail_is_recovered_within_two_retry_intervals",
    ),
    (
        "a lost end-of-sample HEARTBEAT never asked again",
        "    large_send_end_heartbeat(node, pub);\n    uint64_t wait",
        "    uint64_t wait",
        "test_a_lost_tail_is_recovered_within_two_retry_intervals",
    ),
    (
        "a HEARTBEAT announces a large sample still going out",
        "            return record->seq_no - 1;",
        "            return pub->seq_no;",
        "test_a_heartbeat_announces_no_sample_still_going_out",
    ),
    (
        "a datagram lost during its sample's send waits for the send to end",
        "    if (!record->sent && !(cursor->active && cursor->seq_no == record->seq_no &&\n"
        "                           (cursor->next_dest > 0 || index < cursor->next_index))) {",
        "    if (!record->sent) {",
        "test_a_lost_datagram_is_resent_while_its_sample_is_still_going_out",
    ),
    (
        "a retry while a large sample still arrives counts against the writer",
        "        proxy->retry--;",
        "        (void)0;",
        "test_a_busy_writer_is_not_warned_about",
    ),
    (
        "a fixed-size type above a sample refused as a topic",
        "    return valid_sample_size(size) || (size > 0 && size <= largest && decodes_in_place);",
        "    return valid_sample_size(size);",
        "test_a_fixed_size_type_above_a_sample_is_a_large_topic",
    ),
    (
        "a fixed-size large type's copying Subscriber accepted",
        "    return valid_sample_size(size) || (size > 0 && size <= largest && decodes_in_place);",
        "    return valid_sample_size(size) || (size > 0 && size <= largest);",
        "test_a_fixed_size_type_above_a_sample_is_a_large_topic",
    ),
    (
        "a sample wider than the window: WOULD_BLOCK, not TOO_LARGE",
        "    if (count > window) {\n        return tt_RET_TOO_LARGE;",
        "    if (count > window) {\n        return tt_RET_WOULD_BLOCK;",
        "test_keep_all_sample_wider_than_the_window_is_too_large",
    ),
    (
        "the send cursor dropped",
        "    if (tt_Context_schedule(node, tt_get_ns() + wait, large_send_resume, pub)) {",
        "    if (false) {",
        "test_a_full_send_buffer_delays_the_sample_never_loses_it",
    ),
    (
        "a best-effort sample delivered one fragment short",
        "    if (assembly->received != assembly->frag_count) {\n        return;\n    }\n    uint32_t cdr_len",
        "    if (assembly->received + 1 < assembly->frag_count) {\n        return;\n    }\n    uint32_t cdr_len",
        "test_best_effort_without_a_buffer_is_counted_never_torn",
    ),
    (
        "a RELIABLE sample delivered one fragment short",
        "    if (assembly->received == count) {\n        *consumed = count;",
        "    if (assembly->received + 1 >= count) {\n        *consumed = count;",
        "test_reliable_keep_all_at_5_percent_loss",
    ),
    # Aimed at the LARGE_DIRECT branch: on Linux (TT_HAL_RX_DIRECT wherever large samples are) the #else twin is
    # compiled out, and a mutant there changed no byte of the test binary - it "survived" by not existing (2026-10-15).
    (
        "a lent buffer handed back when the callback returns",
        "    } else if (!large_lent(node, buffer)) {\n        large_give_back(node, buffer);",
        "    } else if (true) {\n        large_give_back(node, buffer);",
        "test_a_retained_large_sample_outlives_the_next_one",
    ),
    (
        "a small sample overtakes a waiting large one",
        "    if (size >= 0 && size <= tt_MAX_SAMPLE_LENGTH) {\n        large_commit_pending(node, pub);\n",
        "    if (size >= 0 && size <= tt_MAX_SAMPLE_LENGTH) {\n",
        "test_a_small_sample_never_overtakes_a_waiting_large_one",
    ),
    (
        "a claimed publish overtakes a waiting large sample",
        "    if (pub->large_pending.buffer != NULL) {\n        large_commit_pending(node, pub);\n    }\n#endif\n"
        "    const uint32_t raw_len = CLAIM_FRAMING + length;",
        "#endif\n    const uint32_t raw_len = CLAIM_FRAMING + length;",
        "test_a_claimed_publish_follows_a_waiting_large_sample",
        "test_large_shm",
    ),
    (
        "a waiting KEEP_LAST sample sent rather than replaced",
        "        if (pub->large_pending.buffer != NULL) {\n            // the newer sample replaces it before any of it went\n"
        "            large_record_give_back(node, pub, pub->large_pending.buffer, LARGE_RECORD_CHUNK(&pub->large_pending));",
        "        if (pub->large_pending.buffer != NULL) {\n            large_commit_pending(node, pub);\n"
        "            large_give_back(node, NULL);",
        "test_keep_last_replaces_a_sample_waiting_behind_a_send",
    ),
    (
        "a VOLATILE writer keeps acknowledged buffers",
        "    if (pub->durable || pub->large_count == 0 || !any_peer_ack_matched(pub)) {",
        "    if (true) {",
        "test_a_volatile_writer_hands_acknowledged_buffers_back",
    ),
    (
        "a durable late joiner gets no large samples",
        "    large_send_backlog_before(node, pub, 0, true, &large_next, target);\n",
        "",
        "test_a_durable_writer_backs_a_late_joiner_with_its_large_samples",
    ),
    # A durable backlog that interleaves large and small samples (2026-10-16): each large record goes before the first
    # small sample above it, so the backlog leaves in publish order, and KEEP_LAST's depth counts both kinds together.
    (
        "a mixed backlog's large records all sent after its small ones",
        "        large_send_backlog_before(node, pub, seq_no, false, &large_next, target);\n",
        "",
        "test_a_durable_backlog_interleaving_large_and_small_samples",
    ),
    (
        "a mixed backlog's large record sent after the small one above it",
        "        large_send_backlog_before(node, pub, seq_no, false, &large_next, target);\n#endif\n"
        "        if (!send_cached_record(node, cache->arena + cache_entry->offset, cache_entry->len, false, target)) {\n"
        "            TT_LOG_WARNING(\"Cannot deliver durability backlog seq_no %u\", cache_entry->seq_no);\n        }\n",
        "#endif\n"
        "        if (!send_cached_record(node, cache->arena + cache_entry->offset, cache_entry->len, false, target)) {\n"
        "            TT_LOG_WARNING(\"Cannot deliver durability backlog seq_no %u\", cache_entry->seq_no);\n        }\n"
        "#if tt_LARGE_SAMPLES\n        large_send_backlog_before(node, pub, seq_no, false, &large_next, target);\n#endif\n",
        "test_a_durable_backlog_interleaving_large_and_small_samples",
    ),
    (
        "KEEP_LAST depth: a small sample does not count the large ones",
        "        if (pub->large_count != 0) { // else the cache's own bound is the whole of it\n"
        "            keep_last_bound_across_kinds(node, pub, 1);\n        }\n",
        "",
        "test_a_durable_backlog_interleaving_large_and_small_samples",
    ),
    (
        "KEEP_LAST depth: a large sample does not count the small ones",
        "    keep_last_bound_across_kinds(node, pub, 1);\n    if (pub->large_count >= tt_LARGE_RETAINED)",
        "    if (pub->large_count >= tt_LARGE_RETAINED)",
        "test_a_durable_backlog_interleaving_large_and_small_samples",
    ),
    (
        "KEEP_LAST depth: not brought back once a large send ends",
        "    keep_last_bound_across_kinds(node, pub, 0);\n",
        "",
        "test_a_durable_backlog_interleaving_large_and_small_samples",
    ),
    (
        "KEEP_LAST depth: the newer of the two kinds evicted",
        "        if (small != 0 && (large == NULL || seq_delta(cache->oldest_seq_no, large->seq_no) < 0)) {",
        "        if (small != 0 && (large == NULL || seq_delta(cache->oldest_seq_no, large->seq_no) > 0)) {",
        "test_a_durable_backlog_interleaving_large_and_small_samples",
    ),
    # Encoding while sending (DESIGN.md section 8): a topic's ranged encode, a send call at a time.
    (
        "the sample encoded whole after its first range",
        "    if (encoded == 0) {\n        int32_t whole",
        "    if (true) {\n        int32_t whole",
        "test_each_range_is_encoded_just_before_its_datagrams_leave",
    ),
    (
        "all of it encoded at the first send call",
        "large_end_of(record, cursor->next_index + batch_count - 1))",
        "record->cdr_len)",
        "test_each_range_is_encoded_just_before_its_datagrams_leave",
    ),
    (
        "a send call goes before its range is encoded",
        "        if (!large_encode_through(pub, record, large_end_of(record, cursor->next_index + batch_count - 1))) {",
        "        if (false) {",
        "test_encoding_while_sending_leaves_the_wire_byte_identical",
    ),
    (
        "the rest left unencoded when the send buffer fills",
        "while it drains, rather than a send call at a time with a retry after each.\n"
        "            if (!large_encode_through(pub, record, record->cdr_len)) {\n                *failed = true;\n"
        "                return true;\n            }\n            return false;",
        "while it drains, rather than a send call at a time with a retry after each.\n            return false;",
        "test_a_full_send_buffer_retains_the_whole_encoded_sample",
    ),
    (
        "a declined range not made up by data_encode",
        "            encoded = pub->topic->data_encode(pub->large_source, cdr, size);",
        "            encoded = -1;",
        "test_encoding_while_sending_leaves_the_wire_byte_identical (decline part-way)",
    ),
    (
        "the padding of a sample encoded by range left as it was",
        "        memset(cdr + pad_from, 0, end - pad_from); // sent padded, as a fragment is",
        "        (void)pad_from;",
        "test_encoding_while_sending_leaves_the_wire_byte_identical (1 MB + 3)",
    ),
    (
        "a failed range ignored: the send goes on",
        "            *failed = true;\n            return true;\n        }\n        uint8_t framings",
        "        }\n        uint8_t framings",
        "test_a_sample_that_cannot_be_finished_is_dropped_never_torn",
    ),
    (
        "an unfinished sample kept and served to repairs",
        "        if (failed) {\n            large_drop_unfinished(node, pub, record);\n            continue;\n        }\n",
        "",
        "test_a_sample_that_cannot_be_finished_is_dropped_never_torn",
    ),
    (
        "a sample waiting behind a send encoded by range",
        "    if (pub->topic->data_encode_range == NULL || !sending_now || publisher_unmatched(pub)) {",
        "    if (pub->topic->data_encode_range == NULL || publisher_unmatched(pub)) {",
        "test_a_sample_waiting_behind_a_send_is_encoded_whole",
    ),
    # The busy writer and the recovery estimate (2026-10-16): tt_WriterProxy.probe_busy and repair_arrival_ns.
    (
        "(srtt) a busy writer's recovery timed as the path",
        "        if (!proxy->probe_busy) {",
        "        if (true) {",
        "test_a_busy_writers_send_is_not_timed_as_the_path",
    ),
    (
        "(srtt) originals that came with the gap taken for a send going on",
        "        if (proxy->probe_ns != 0 && now > proxy->probe_ns) {",
        "        if (proxy->probe_ns != 0 && now >= proxy->probe_ns) {",
        "test_a_quiet_paths_round_trip_is_still_timed",
    ),
    (
        "(srtt) the busy mark kept past its probe",
        "    proxy->probe_ns = now != 0 ? now : 1U;\n    proxy->probe_busy = false;\n",
        "    proxy->probe_ns = now != 0 ? now : 1U;\n",
        "test_a_quiet_paths_round_trip_is_still_timed",
    ),
    (
        "(srtt) a repair marks the probe busy",
        "        proxy->original_arrival_ns = now;\n        if (proxy->probe_ns != 0 && now > proxy->probe_ns) {\n"
        "            proxy->probe_busy = true;\n        }\n    }\n",
        "        proxy->original_arrival_ns = now;\n    }\n    if (proxy->probe_ns != 0 && now > proxy->probe_ns) {\n"
        "        proxy->probe_busy = true;\n    }\n",
        "test_a_quiet_paths_round_trip_is_still_timed",
    ),
    (
        "(srtt) a retry while the writer sends goes as any other",
        "        if (sending) {\n            bool due",
        "        if (false) {\n            bool due",
        "test_a_busy_writers_send_is_not_timed_as_the_path (writer datagrams)",
    ),
    (
        "(srtt) a writer answering nothing while it sends asked again",
        "            if (!due || !answering || !names_any_open(proxy, highest, skips, skip_count)) {",
        "            if (!due || !names_any_open(proxy, highest, skips, skip_count)) {",
        "test_a_busy_writers_send_is_not_timed_as_the_path (writer datagrams)",
    ),
    (
        "(srtt) a retry while the writer sends names repairs in flight",
        "            skip_count = repairs_in_flight(node, proxy, highest, checked_ns, skips);",
        "            skip_count = 0;",
        "test_a_sending_writer_is_not_asked_for_repairs_on_their_way",
    ),
    (
        "(srtt) the reliable retry's ceiling a multiple of srtt alone",
        "    uint64_t unit = srtt > granularity ? srtt : granularity;\n    uint64_t ceiling = unit * (uint64_t)reliable->max_srtt_multiple",
        "    uint64_t unit = srtt;\n    uint64_t ceiling = unit * (uint64_t)reliable->max_srtt_multiple",
        "test_retry_interval_estimate_converges_and_is_bounded",
        "test_reliable_pubsub",
    ),
    (
        "a HEARTBEAT rides the broadcast content batched ahead of it",
        "    if (node->tx_tail != sizeof(struct tt_Header)) {\n        flush_batched(node);\n    }\n    return node->tx_tail",
        "    return node->tx_tail",
        "test_a_heartbeat_takes_the_path_of_its_data",
    ),
]

# Survivors with no unit test that sees them (2026-10-16, srtt_busy_writer_pc_ab.sh): the cadence of the retries while
# the writer still sends (tt_WriterProxy.sending_srtt_ns, retry_sent_ns, original_arrival_ns) answers the PC reader's
# collapses - its socket overflowing, about one 4 MB run in five - where the change ran deeper than the parent without
# it; test_large_sample's sim has no receiver that overflows, and each of these passes it. Kept out of the sweep so it
# stays green, and listed so that a sim with an overflowing reader can bring them back.
UNGUARDED = [
    (
        "(srtt) a retry while the writer sends asks at the path's interval",
        "            bool due = checked_ns - proxy->retry_sent_ns >= retry_cadence_while_sending(node, proxy, interval);",
        "            bool due = true;",
        "test_a_sending_writer_is_not_asked_for_repairs_on_their_way",
    ),
    (
        "(srtt) the sending estimate never fed",
        "        rtt_estimate_fold(&proxy->sending_srtt_ns, &proxy->sending_rttvar_ns, took);\n",
        "",
        "test_a_sending_writer_is_not_asked_for_repairs_on_their_way",
    ),
    (
        "(srtt) a repair taken for the writer still sending",
        "    if (!node->rx_targeted) {\n        proxy->original_arrival_ns = now;",
        "    {\n        proxy->original_arrival_ns = now;",
        "test_a_busy_writers_send_is_not_timed_as_the_path",
    ),
    (
        "(srtt) a writer answering while it sends is not asked again",
        "            bool answering = proxy->repair_arrival_ns != 0 && checked_ns - proxy->repair_arrival_ns < interval;",
        "            bool answering = false;",
        "test_a_quiet_paths_round_trip_is_still_timed (a writer answering while it sends)",
    ),
]


def build_and_run(binary, control_digest=None):
    path = TESTS / binary
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(path.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True, cwd=ROOT / "platform/linux",
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    if unchanged(control_digest, path):
        return NOT_COMPILED, ""
    try:
        # The binary takes about a second; a mutant that makes a simulation spin is cut off at a minute.
        run = subprocess.run([str(path)], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired as hung:
        # A mutant that loops forever is killed, and said to be: without the bound the first one hung the sweep with
        # the mutant still applied to src/tickle.c (tests/mutants_frag_fast_path.py).
        return "HANG", str(hung.stdout or "")[-1000:]
    if run.returncode < 0:
        return "CRASH", run.stdout[-1000:] + run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def failing_lines(output, binary):
    return [line for line in output.splitlines() if f"{binary}.c:" in line][:3]


def main():
    backup = tempfile.mkdtemp(prefix="large_sample_mutants_")
    saved = pathlib.Path(backup) / "tickle.c"
    shutil.copy2(ROOT / SRC, saved)

    def restore():
        # copy(), then touch: copy2 would keep the original mtime, older than the mutant's objects, and make would then
        # reuse a binary built from mutated source (tests/mutants_shm_stage1.py, 2026-10-02).
        shutil.copy(saved, ROOT / SRC)
        (ROOT / SRC).touch()

    signal.signal(signal.SIGTERM, lambda _s, _f: sys.exit(1))
    failures = 0
    digests = {}
    try:
        # Every binary a mutant is judged by must pass unmutated, or its mutants cannot be told from its own failures.
        for binary in sorted({entry[4] if len(entry) > 4 else DEFAULT_BINARY for entry in MUTANTS}):
            verdict, output = build_and_run(binary)
            print(f"[control]  unmutated {binary:44s} -> {verdict}")
            if verdict != "PASS":
                print(output[-2000:])
                print("control did not pass; the sweep cannot decide anything")
                return 1
            digests[binary] = code_digest(TESTS / binary)

        only = os.environ.get("MUTANTS", "")
        for name, old, new, test, *rest in MUTANTS:
            binary = rest[0] if rest else DEFAULT_BINARY
            if only and only not in name:
                continue
            path = ROOT / SRC
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR]    {name:55s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            verdict, output = build_and_run(binary, digests[binary])
            restore()
            ok = verdict in ("FAIL", "CRASH", "HANG")
            failures += 0 if ok else 1
            print(f"[{label(verdict, ok):8s}] {name:55s} -> {verdict} (guard: {binary}: {test})", flush=True)
            for line in failing_lines(output, binary) if ok else []:
                print(f"           {line}")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)

    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
