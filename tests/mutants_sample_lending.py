#!/usr/bin/env python3
"""Mutant sweep for receive-buffer lending (DESIGN.md section 10; tests/test_sample_lending.c).

Each entry removes or bends exactly one piece of tt_Sample_retain() / tt_Sample_release() and the in-place segment
drain, and names the test that must go red for it. A mutant that survives is a piece no test decides - either the test
is missing or the piece is doing nothing, and both are findings. The verdict also checks that the named test is among
the failures, so a mutant killed by an assertion elsewhere does not count for the piece it names; a crash (the binary
killed by a signal) counts, and is printed as CRASH.

Run from the repository root. MUTANTS=<substring> runs only the mutants whose name contains it (the control always
runs). Refuses to start (exit 2) if any anchor is not in its file exactly once - "the release frees the slot" matched
nothing from 6f0c8a93, when the release became segment_unhold(), and the sweep counted it as one error among the rest -
or if a file it edits is already modified. Every child is bounded (tests/mutant_guard.py: 2 GB of address space, a
deadline, at most 256 KB of its output kept); every file is restored after each mutant, on an exception and on
SIGINT/SIGTERM/SIGHUP, with a fresh mtime so make rebuilds.

Not covered: the writer's lend.full_retained count (segment_full_by_hold()). Since 6f0c8a93 a writer passes a held
slot instead of being refused by it, and every build's ring has at least SEGMENT_HELD_MIN_SLOTS slots, so no test here
can make a hold refuse a writer; its mutant survived as written and was removed.
"""
import os
import pathlib
import sys

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, run_capped, usage_line

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_sample_lending"
TEST_SRC = ROOT / "tests/test_sample_lending.c"

MUTANTS = [
    (
        "the drain keeps a held slot",
        "src/tickle.c",
        "    if (node->lend.held_slots != 0 && lend_holds_slot(node, header, index)) {",
        "    if (false) {",
        "test_a_retained_slot_survives_a_lap",
    ),
    (
        "a slot retain counted",
        "src/tickle.c",
        "        region = node->own_segment;\n        node->lend.held_slots++;",
        "        region = node->own_segment;",
        "test_a_retained_slot_survives_a_lap",
    ),
    (
        "the release frees the slot",
        "src/tickle.c",
        # Since 6f0c8a93 the release turns the slot's held mark into "free" by CAS (segment_unhold()), not a store.
        "    segment_unhold(header, released->index);\n#else",
        "#else",
        "test_a_retained_slot_survives_a_lap",
    ),
    (
        "a batch's slot kept until its last sample",
        "src/tickle.c",
        "    if (lend_holds_slot(node, header, released->index)) {\n        return; // another sample",
        "    if (false) {\n        return; // another sample",
        "test_a_batch_record_is_released_with_its_last_sample",
    ),
    (
        "a release inside the callback leaves the record to the drain",
        "src/tickle.c",
        "    if (node->lend.rx_kind == tt_LEND_SLOT && node->lend.rx_index == released->index) {\n        return; // still",
        "    if (false) {\n        return; // still",
        "test_retain_and_release_in_one_callback",
    ),
    (
        "the socket moved to a spare buffer",
        "src/tickle.c",
        "            node->lend.landing = (uint8_t)spare;",
        "            (void)spare;",
        "test_socket_retain_keeps_its_buffer",
    ),
    (
        "the spare buffer is not the retained one",
        "src/tickle.c",
        "        if (number != current && !lend_holds_buffer(node, number)) {",
        "        if (!lend_holds_buffer(node, number)) {",
        "test_socket_retain_keeps_its_buffer",
    ),
    (
        "no spare buffer refuses the retain",
        "src/tickle.c",
        "            if (spare < 0) {\n                node->lend.exhausted++;",
        "            if (false) {\n                node->lend.exhausted++;",
        "test_socket_retain_without_a_pool_fails_cleanly",
    ),
    (
        "the handle's generation checked",
        "src/tickle.c",
        "entry->kind == tt_LEND_FREE || entry->generation != (handle >> 8U)) {",
        "entry->kind == tt_LEND_FREE) {",
        "test_bad_releases_are_refused_and_touch_nothing",
    ),
    (
        "a free entry refuses a release",
        "src/tickle.c",
        "entry->kind == tt_LEND_FREE || entry->generation != (handle >> 8U)) {",
        "entry->generation != (handle >> 8U)) {",
        "test_bad_releases_are_refused_and_touch_nothing",
    ),
    (
        "the handle table bounded",
        "src/tickle.c",
        "    if (entry_index < 0) {\n        node->lend.exhausted++;",
        "    if (entry_index < 0 && false) {\n        node->lend.exhausted++;",
        "test_a_full_handle_table_fails_cleanly",
    ),
    (
        "only bytes in the datagram are lent",
        "src/tickle.c",
        "    if (kind == tt_LEND_FREE || delivery->payload < base || delivery->length > node->lend.rx_length ||\n"
        "        (size_t)(delivery->payload - base) > node->lend.rx_length - delivery->length) {",
        "    if (kind == tt_LEND_FREE) {",
        "test_a_reassembled_sample_is_not_lent",
    ),
    (
        "only inside this Subscriber's callback",
        "src/tickle.c",
        "    if (delivery == NULL || delivery->sub != sub) {",
        "    if (delivery == NULL) {",
        "test_retain_outside_its_callback_is_refused",
    ),
    (
        "the delivery recorded for the callback",
        "src/tickle.c",
        "#if tt_SAMPLE_LENDING\n    node->lend.delivery = &lend;\n#endif\n"
        "    sub->callback(sub, timestamp, (uint16_t)seq_no, (struct tt_Data*)data);",
        "    sub->callback(sub, timestamp, (uint16_t)seq_no, (struct tt_Data*)data);",
        "test_a_retained_slot_survives_a_lap",
    ),
    (
        "the segment release waits for a held slot",
        "src/tickle.c",
        "        if (node->lend.held_slots != 0 || lend_reading_slot(node)) {\n"
        "            node->lend.segment_release_deferred = true;",
        "        if (false) {\n            node->lend.segment_release_deferred = true;",
        "test_the_segment_outlives_a_held_slot",
    ),
    (
        "the segment release waits for the record read in place",
        "src/tickle.c",
        "        if (node->lend.held_slots != 0 || lend_reading_slot(node)) {\n"
        "            node->lend.segment_release_deferred = true;",
        "        if (node->lend.held_slots != 0) {\n            node->lend.segment_release_deferred = true;",
        "test_a_farewell_read_in_place_releases_the_ring_after",
    ),
    (
        "the drain finishes a release it put off",
        "src/tickle.c",
        "            if (node->lend.segment_release_deferred && lend_finish_release(node)) {",
        "            if (false) {",
        "test_a_farewell_read_in_place_releases_the_ring_after",
    ),
    (
        "the deferred release waits for the last release",
        "src/tickle.c",
        "    if (!node->lend.segment_release_deferred || node->lend.held_slots != 0 || "
        "lend_reading_slot(node)) {",
        "    if (!node->lend.segment_release_deferred) {",
        "test_the_segment_outlives_a_held_slot",
    ),
    (
        "destroy forgets held samples",
        "src/tickle.c",
        "    for (uint32_t k = 0; k < tt_SAMPLE_RETAIN_MAX; k++) {\n#if tt_LARGE_SAMPLES\n        // A retained large sample's",
        "    for (uint32_t k = 0; k < 0; k++) {\n#if tt_LARGE_SAMPLES\n        // A retained large sample's",
        "test_destroy_forgets_held_samples",
    ),
]


def test_spans():
    """(first line, last line, name) of every test function in the test file, to name the test a failure is in."""
    spans = []
    lines = TEST_SRC.read_text().split("\n")
    for number, line in enumerate(lines, 1):
        if line.startswith("static void test_") and line.endswith("{"):
            name = line[len("static void "):line.index("(")]
            if spans:
                spans[-1][1] = number - 1
            spans.append([number, len(lines), name])
    return spans


def failing_tests(output, spans):
    names = set()
    for line in output.split("\n"):
        if "test_sample_lending.c:" not in line:
            continue
        try:
            number = int(line.split("test_sample_lending.c:")[1].split(":")[0])
        except ValueError:
            continue
        names |= {name for first, last, name in spans if first <= number <= last}
    return names


def build_and_run(control=None):
    build = run_capped(["make", "-s", "-j2", str(BIN.relative_to(ROOT / "platform/linux"))], timeout=900,
                       cwd=ROOT / "platform/linux")
    if build.returncode != 0:
        return "BUILD_FAILED", build.output[-2000:]
    if unchanged(control, BIN):
        return NOT_COMPILED, ""
    run = run_capped([str(BIN)], timeout=300)
    if run.timed_out:
        return "HANG", run.output  # a failure too, but one no test line names: printed as such
    if run.returncode is None or run.returncode < 0:
        return "CRASH", run.output  # killed by a signal: a failure no test line can name
    return ("PASS" if run.returncode == 0 else "FAIL"), run.output


def main():
    be_nice()
    check_anchors(ROOT, [(name, rel, old, new) for name, rel, old, new, _test in MUTANTS])
    spans = test_spans()
    failures = 0
    with SourceGuard(ROOT, {m[1] for m in MUTANTS}) as guard:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                              -> {verdict}", flush=True)
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
            red = failing_tests(output, spans)
            # A crash or a hang counts: the piece's absence is what stopped the binary, though no assertion names the
            # test.
            ok = (verdict == "FAIL" and test in red) or verdict in ("CRASH", "HANG")
            failures += 0 if ok else 1
            print(f"[{label(verdict, ok):8s}] {name:48s} -> {verdict} in {sorted(red) or '-'} (guard: {test})",
                  flush=True)
            if not ok:
                print(output[-1500:])

    print(usage_line())
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
