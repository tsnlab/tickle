#!/usr/bin/env python3
"""Mutant sweep for large-message stage 2, step A (DESIGN.md section 8, "Stage 2", pre-registration L1).

Each entry breaks exactly one thing the stage-2 code relies on and names the test in tests/test_large_sample.c that
must go red for it - the mutants the pre-registration names (an off-by-one in the 16-bit offset arithmetic, no
end-of-sample HEARTBEAT, WOULD_BLOCK where a sample can never fit, the send cursor dropped, a torn sample delivered, a
lent buffer reused before its release), and a few more for conditions the tests claim to cover. A mutant that still
passes means either the test does not test what its name says, or the condition decides nothing and should not be in
the code. L1.2's own mutant (small samples sent as large) is in tests/wire_identity_largemsg.sh, which compares
against the parent's bytes.

Run from the repository root. Restores every file it touches, whatever happens. MUTANTS=<substring> runs only the
mutants whose name contains it (the control always runs).
"""
import pathlib
import shutil
import signal
import subprocess
import sys
import os
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_large_sample"
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
        "test_a_lost_datagram_is_resent_while_its_sample_is_still_going_out",
    ),
    (
        "a datagram lost during its sample's send waits for the send to end",
        "    if (!record->sent && !(cursor->seq_no == record->seq_no && (cursor->next_dest > 0 || index < cursor->next_index))) {",
        "    if (!record->sent) {",
        "test_a_lost_datagram_is_resent_while_its_sample_is_still_going_out",
    ),
    (
        "a retry while a large sample still arrives counts against the writer",
        "        proxy->retry--;",
        "        (void)0;",
        "test_a_busy_writer_is_waited_for_not_given_up",
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
        "    if (tt_Context_schedule(node, tt_get_ns() + cursor->retry_ns, large_send_resume, pub)) {",
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
    (
        "a lent buffer handed back when the callback returns",
        "    if (!large_lent(node, assembly->buffer)) {\n        large_give_back(node, assembly->buffer);",
        "    if (true) {\n        large_give_back(node, assembly->buffer);",
        "test_a_retained_large_sample_outlives_the_next_one",
    ),
    (
        "a small sample overtakes a waiting large one",
        "        large_commit_pending(node, pub);\n",
        "",
        "test_a_small_sample_never_overtakes_a_waiting_large_one",
    ),
    (
        "a waiting KEEP_LAST sample sent rather than replaced",
        "        if (pub->large_pending.buffer != NULL) {\n            large_give_back(node, pub->large_pending.buffer);",
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
        "    large_send_backlog_before(node, pub, UINT32_MAX, &large_next, target);\n",
        "",
        "test_a_durable_writer_backs_a_late_joiner_with_its_large_samples",
    ),
]


def build_and_run():
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True, cwd=ROOT / "platform/linux",
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    try:
        # The binary takes about a second; a mutant that makes a simulation spin is cut off at a minute.
        run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired as hung:
        # A mutant that loops forever is killed, and said to be: without the bound the first one hung the sweep with
        # the mutant still applied to src/tickle.c (tests/mutants_frag_fast_path.py).
        return "HANG", str(hung.stdout or "")[-1000:]
    if run.returncode < 0:
        return "CRASH", run.stdout[-1000:] + run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def failing_lines(output):
    return [line for line in output.splitlines() if "test_large_sample.c:" in line][:3]


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
    try:
        verdict, output = build_and_run()
        print(f"[control]  unmutated                                              -> {verdict}")
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1

        only = os.environ.get("MUTANTS", "")
        for name, old, new, test in MUTANTS:
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
            verdict, output = build_and_run()
            restore()
            ok = verdict in ("FAIL", "CRASH", "HANG")
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name:55s} -> {verdict} (guard: {test})", flush=True)
            for line in failing_lines(output) if ok else []:
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
