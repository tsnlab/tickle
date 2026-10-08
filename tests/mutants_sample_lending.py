#!/usr/bin/env python3
"""Mutant sweep for receive-buffer lending (DESIGN.md section 10; tests/test_sample_lending.c).

Each entry removes or bends exactly one piece of tt_Sample_retain() / tt_Sample_release() and the in-place segment
drain, and names the test that must go red for it. A mutant that survives is a piece no test decides - either the test
is missing or the piece is doing nothing, and both are findings. The verdict also checks that the named test is among
the failures, so a mutant killed by an assertion elsewhere does not count for the piece it names; a crash (the binary
killed by a signal) counts, and is printed as CRASH.

Run from the repository root. Restores every file it touches, whatever happens.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

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
        "    __atomic_store_n(&slot_header->sequence, released->index + header->slots, __ATOMIC_RELEASE);\n#else",
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
        "the writer's hold counter",
        "src/tickle.c",
        "        if (segment_full_by_hold(segment)) {\n            node->lend.full_retained++;",
        "        if (segment_full_by_hold(segment)) {\n            (void)0;",
        "test_a_retained_slot_survives_a_lap",
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
        "        if (node->lend.held_slots != 0 || node->lend.rx_kind == tt_LEND_SLOT) {\n"
        "            node->lend.segment_release_deferred = true;",
        "        if (false) {\n            node->lend.segment_release_deferred = true;",
        "test_the_segment_outlives_a_held_slot",
    ),
    (
        "the segment release waits for the record read in place",
        "src/tickle.c",
        "        if (node->lend.held_slots != 0 || node->lend.rx_kind == tt_LEND_SLOT) {\n"
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
        "node->lend.rx_kind == tt_LEND_SLOT) {",
        "    if (!node->lend.segment_release_deferred) {",
        "test_the_segment_outlives_a_held_slot",
    ),
    (
        "destroy forgets held samples",
        "src/tickle.c",
        "    for (uint32_t k = 0; k < tt_SAMPLE_RETAIN_MAX; k++) {\n        node->lend.entries[k].kind = tt_LEND_FREE;",
        "    for (uint32_t k = 0; k < 0; k++) {\n        node->lend.entries[k].kind = tt_LEND_FREE;",
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


def build_and_run():
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True, cwd=ROOT / "platform/linux",
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    run = subprocess.run([str(BIN)], capture_output=True, text=True)
    if run.returncode < 0:
        return "CRASH", run.stdout + run.stderr  # killed by a signal: a failure no test line can name
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    backup = tempfile.mkdtemp(prefix="lend_mutants_")
    touched = {m[1] for m in MUTANTS}
    for rel in touched:
        shutil.copy2(ROOT / rel, pathlib.Path(backup) / rel.replace("/", "_"))

    def restore():
        for rel in touched:
            # copy() and touch, not copy2(): a restored file must look newer than objects built from the mutant.
            shutil.copy(pathlib.Path(backup) / rel.replace("/", "_"), ROOT / rel)
            (ROOT / rel).touch()

    spans = test_spans()
    failures = 0
    try:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                              -> {verdict}")
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1

        for name, rel, old, new, test in MUTANTS:
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR]    {name:48s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            assert path.read_text().count(old) == 0, "substitution did not apply"
            verdict, output = build_and_run()
            restore()
            red = failing_tests(output, spans)
            # A crash counts: the piece's absence is what made the binary die, though no assertion names the test.
            ok = (verdict == "FAIL" and test in red) or verdict == "CRASH"
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name:48s} -> {verdict} in {sorted(red) or '-'} (guard: {test})")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)

    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
