#!/usr/bin/env python3
"""Mutant sweep for the fence on the switch from UDP to a peer's segment (tests/test_segment_switch.c).

Each mutant removes exactly one claim the fence makes, and test_segment_switch must fail on every one of them. The
parent - this tree, unmutated - must pass first, or the sweep decides nothing.

A mutant that hangs the test (a reader that spins on a held fence without ever sleeping, so the mock clock never moves)
is a failure of the test binary, which is what it should be: it is stopped at TIMEOUT_S and counted as FAIL.

Not covered here: segment_read()'s copy of the check, which only a tt_SAMPLE_LENDING=0 build compiles. Its mutant is
the same one-line change as segment_take()'s, which is covered.

Run from the repository root. tests/mutant_guard.py refuses the sweep unless every anchor occurs exactly once, bounds
every child (2 GB, a deadline, capped output) and restores src/tickle.c whatever happens, SIGINT/SIGTERM included.
"""
import os
import pathlib
import sys

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, killed_by, run_capped, usage_line

ROOT = pathlib.Path(__file__).resolve().parents[1]
LINUX = ROOT / "platform/linux"
BIN = LINUX / "obj/debug/tests/test_segment_switch"
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
        # segment_take()'s copy (the comment line makes it the only match). Since 59a03ee7 the same branch also leaves
        # a reference record (tt_SEGMENT_SPAN_REF) at the head; the mutant keeps that and drops only the fence.
        "    uint16_t span = slot_header->seq_span; // as segment_read(): 0 and out-of-range spans read as 1, a fence stays\n"
        "    if (span - 1U >= tt_FRAG_MAX_COUNT) {\n"
        "        if (span == tt_SEGMENT_SPAN_FENCE || span == tt_SEGMENT_SPAN_REF) {",
        "    uint16_t span = slot_header->seq_span; // as segment_read(): 0 and out-of-range spans read as 1, a fence stays\n"
        "    if (span - 1U >= tt_FRAG_MAX_COUNT) {\n"
        "        if (span == tt_SEGMENT_SPAN_REF) {",
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


def build_and_run(control=None):
    build = run_capped(["make", "-s", str(BIN.relative_to(LINUX))], timeout=900, cwd=LINUX)
    if build.returncode != 0:
        return "BUILD_FAILED", build.output[-2000:]
    if unchanged(control, BIN):
        return NOT_COMPILED, ""
    run = run_capped([str(BIN)], timeout=TIMEOUT_S, cwd=LINUX)
    if run.timed_out:
        return "FAIL", f"(hung: stopped after {TIMEOUT_S} s)"
    out = "\n".join(line for line in run.output.splitlines() if not line.startswith("["))
    return ("PASS" if run.returncode == 0 else "FAIL"), out


def main():
    be_nice()
    # Every anchor must occur exactly once before anything runs: one that matches nothing is a mutant that cannot fail
    # ("the drain leaves a fence at the head" matched nothing from 59a03ee7 until it was re-anchored).
    check_anchors(ROOT, [(name, rel, old, new) for name, rel, old, new, _test in MUTANTS])
    failures = 0
    with SourceGuard(ROOT, {m[1] for m in MUTANTS}) as guard:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                                      -> {verdict}", flush=True)
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
            ok = verdict == "FAIL"
            failures += 0 if ok else 1
            print(f"[{label(verdict, ok):8s}] {name:58s} -> {verdict} (wanted FAIL, guard: {test})", flush=True)
            if ok:
                print(f"  killed by: {', '.join(killed_by(output, ROOT)) or '(no failing check printed)'}")
            else:
                print(output[-1500:])
    run_capped(["make", "-s", str(BIN.relative_to(LINUX))], timeout=900, cwd=LINUX)
    print(usage_line())
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
