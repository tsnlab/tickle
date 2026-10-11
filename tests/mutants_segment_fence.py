#!/usr/bin/env python3
"""Mutant sweep for the fence on the switch from UDP to a peer's segment (tests/test_segment_switch.c).

Each mutant removes exactly one claim the fence makes, and test_segment_switch must fail on every one of them. The
parent - this tree, unmutated - must pass first, or the sweep decides nothing.

A mutant that hangs the test (a reader that spins on a held fence without ever sleeping, so the mock clock never moves)
is a failure of the test binary, which is what it should be: it is stopped at TIMEOUT_S and counted as FAIL.

Not covered here: segment_read()'s copy of the check, which only a tt_SAMPLE_LENDING=0 build compiles. Its mutant is
the same one-line change as segment_take()'s, which is covered.

Run from the repository root. Restores every file it touches, whatever happens.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_segment_switch"
TIMEOUT_S = 60

MUTANTS = [
    (
        "the writer fences the switch at all",
        "src/tickle.c",
        "    if (header == NULL || !after_udp || segment_fence(node, header, ip, port)) {",
        "    if (true) {",
        "test_the_switch_from_udp_loses_nothing",
    ),
    (
        "the writer fences only an attach that follows UDP",
        "src/tickle.c",
        "    const bool after_udp = entry->missing && entry->ip == ip && entry->port == port;",
        "    const bool after_udp = true;",
        "test_a_revalidation_sends_no_fence",
    ),
    (
        "the fence datagram is sent",
        "src/tickle.c",
        "    if (tt_send_to(node, fence, sizeof(fence), ip, port) < 0) {",
        "    if (false) {",
        "test_the_switch_from_udp_loses_nothing (passed on its datagram, not the deadline)",
    ),
    (
        "a full ring refuses the switch",
        "src/tickle.c",
        "    if (slot == NULL) {\n        return false;\n    }\n    memcpy(segment_slot_payload(slot), fence, sizeof(fence));",
        "    if (slot == NULL) {\n        return true;\n    }\n    memcpy(segment_slot_payload(slot), fence, sizeof(fence));",
        "test_a_full_ring_refuses_the_switch",
    ),
    (
        "the drain leaves a fence at the head",
        "src/tickle.c",
        "    uint16_t span = slot_header->seq_span; // as segment_read(): 0 and out-of-range spans read as 1, a fence stays\n"
        "    if (span - 1U >= tt_FRAG_MAX_COUNT) {\n        if (span == tt_SEGMENT_SPAN_FENCE) {",
        "    uint16_t span = slot_header->seq_span; // as segment_read(): 0 and out-of-range spans read as 1, a fence stays\n"
        "    if (span - 1U >= tt_FRAG_MAX_COUNT) {\n        if (false) {",
        "test_the_switch_from_udp_loses_nothing",
    ),
    (
        "a fence holds until its datagram",
        "src/tickle.c",
        "    bool seen = slot->length != tt_SEGMENT_FENCE_LENGTH || fence[0] != (uint8_t)tt_SEGMENT_FENCE_MARKER;",
        "    bool seen = true;",
        "test_the_switch_from_udp_loses_nothing",
    ),
    (
        "the fence datagram is taken off the socket",
        "src/tickle.c",
        "    if (tail == tt_SEGMENT_FENCE_LENGTH && buffer[0] == (uint8_t)tt_SEGMENT_FENCE_MARKER) {",
        "    if (false) {",
        "test_the_switch_from_udp_loses_nothing (passed, not expired)",
    ),
    (
        "a datagram read first is remembered",
        "src/tickle.c",
        "        if (node->segment_fences_seen[k] == key) {",
        "        if (false) {",
        "test_a_fence_whose_datagram_came_first_passes_at_once",
    ),
    (
        "the generation tells fences apart",
        "src/tickle.c",
        "    return ((((uint32_t)fence[1]) << BITS_IN_1BYTE) | fence[2]) + 1U;",
        "    return (((uint32_t)fence[1]) << BITS_IN_1BYTE) + 1U;",
        "test_a_late_fence_does_not_pass_a_newer_one",
    ),
    (
        "a held fence is given up at its deadline",
        "src/tickle.c",
        "        if (now < node->segment_fence_deadline_ns) {",
        "        if (true) {",
        "test_a_lost_fence_holds_the_ring_until_its_deadline",
    ),
    (
        "the deadline is not reached early",
        "src/tickle.c",
        "        if (now < node->segment_fence_deadline_ns) {",
        "        if (false) {",
        "test_a_lost_fence_holds_the_ring_until_its_deadline",
    ),
    (
        "a sleep with a fence held is bounded by its deadline (its scheduler entry)",
        "src/tickle.c",
        "            if (node->scheduler_tail + 1 < tt_MAX_SCHEDULER_LENGTH) {\n"
        "                sched_heap_insert(node, node->segment_fence_deadline_ns, segment_fence_due, NULL);",
        "            if (false) {\n"
        "                sched_heap_insert(node, node->segment_fence_deadline_ns, segment_fence_due, NULL);",
        "test_a_lost_fence_holds_the_ring_until_its_deadline",
    ),
    (
        "a held fence sleeps on the socket rather than draining again",
        "src/tickle.c",
        "    return *took || node->segment_fence_deadline_ns == 0;",
        "    return true;",
        "test_a_lost_fence_holds_the_ring_until_its_deadline",
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
        run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return "FAIL", f"(hung: stopped after {TIMEOUT_S} s)"
    out = "\n".join(line for line in (run.stdout + run.stderr).splitlines() if not line.startswith("["))
    return ("PASS" if run.returncode == 0 else "FAIL"), out


def main():
    backup = tempfile.mkdtemp(prefix="fence_mutants_")
    touched = {m[1] for m in MUTANTS}
    for rel in touched:
        shutil.copy2(ROOT / rel, pathlib.Path(backup) / rel.replace("/", "_"))

    def restore():
        for rel in touched:
            # copy() and touch, not copy2(): a restored file must look newer than the mutant's objects, or make reuses
            # them and the next build tests the mutant (tests/mutants_liveliness_deferral.py says what that cost).
            shutil.copy(pathlib.Path(backup) / rel.replace("/", "_"), ROOT / rel)
            (ROOT / rel).touch()

    failures = 0
    try:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                                      -> {verdict}")
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1

        for name, rel, old, new, test in MUTANTS:
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                # A substitution that matches nothing is the sweep's own test that cannot fail: an error, not a skip.
                print(f"[ERROR]    {name:58s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            assert path.read_text().count(old) == 0 or old in new, "substitution did not apply"
            verdict, output = build_and_run()
            restore()
            ok = verdict == "FAIL"
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name:58s} -> {verdict} (wanted FAIL, guard: {test})")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
        subprocess.run(["make", "-s", "-C", str(ROOT / "platform/linux"),
                        str(BIN.relative_to(ROOT / "platform/linux"))], capture_output=True)

    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
