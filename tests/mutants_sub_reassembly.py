#!/usr/bin/env python3
"""Mutant sweep for the per-Subscriber reassembly slots (docs/DESIGN.md section 8, "Reassembly slots"; the user,
2026-10-27): every Subscriber that puts fragmented samples together in slots owns one, and the node's pool is shared
overflow. One Subscriber's delivery must depend only on its own QoS, the network and its own reading.

Each entry removes exactly one condition or mechanism of that ownership and names the test in tests/test_data_frag.c
that must go red for it. A mutant that still passes means either the test does not test what its name says, or the
condition decides nothing and should not be in the code.

Run from the repository root: python3 tests/mutants_sub_reassembly.py. Refuses to start if any anchor is not in
tickle.c exactly once, or if tickle.c is already modified. Every child is bounded (tests/mutant_guard.py: 2 GB of
address space, a deadline, at most 256 KB of its output kept); tickle.c is restored after each mutant, on an exception
and on SIGINT/SIGTERM/SIGHUP, with a fresh mtime. MUTANTS=<substring> runs only the mutants whose name contains it.
"""
import os
import pathlib
import sys

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, killed_by, run_capped, usage_line

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_data_frag"
SRC = "src/tickle.c"

MUTANTS = [
    (
        "own slot first",
        "    if (own != NULL && frag_slot_spent(own)) {\n        slot = own;",
        "    if (false) {\n        slot = own;",
        "test_best_effort_beside_reliable_none_when_every_slot_is_pinned",
    ),
    (
        "a newer BEST_EFFORT sample takes the own slot",
        "    } else if (own != NULL && frag_newest_takes_own(sub, own, source, entity_id, seq_no)) {",
        "    } else if (false) {",
        "test_older_sample_moved_out_of_the_own_slot_completes",
    ),
    (
        "the move copies what was placed",
        "    _tt_memcpy(dst, src, end < from->capacity ? end : from->capacity);\n",
        "",
        "test_older_sample_moved_out_of_the_own_slot_completes",
    ),
    (
        "a pinned slot is never reclaimed",
        "        if (!frag_slot_busy(slot) || frag_slot_pinned(slot)) {\n            continue;",
        "        if (!frag_slot_busy(slot)) {\n            continue;",
        "test_reliable_none_recorded_sample_survives_a_full_pool",
    ),
    (
        "nobody's slot never displaces an owned one",
        "        } else if (sub != NULL || slot->owner == NULL) {",
        "        } else {",
        "test_full_pool_abandons_the_oldest_and_counts_it",
    ),
    (
        "BEST_EFFORT keeps a newer sample over an older",
        "    return !frag_unbuffered_reliable(sub) && slot->source == source && slot->entity_id == entity_id &&",
        "    return false && slot->source == source && slot->entity_id == entity_id &&",
        "test_full_pool_abandons_the_oldest_and_counts_it",
    ),
    (
        "only an unowned pool slot is taken over",
        "frag_slot_is(slot, source, entity_id, seq_no) && (slot->owner == sub || slot->owner == NULL)) {",
        "frag_slot_is(slot, source, entity_id, seq_no)) {",
        "test_two_best_effort_subscribers_of_one_writer_each_reassemble",
    ),
    (
        "a slot's sample goes to its owner only",
        "    if (ctx->only != NULL && ctx->only != sub) {",
        "    if (false) {",
        "test_two_best_effort_subscribers_of_one_writer_each_reassemble",
    ),
    (
        "pinning is the no-buffer RELIABLE owner's",
        "    if (sub == NULL || !frag_slot_busy(slot) || !frag_unbuffered_reliable(sub)) {",
        "    if (sub == NULL || !frag_slot_busy(slot)) {",
        "test_full_pool_abandons_the_oldest_and_counts_it",
    ),
    (
        "a passed BEST_EFFORT sample claims nothing",
        "        if (proxy != NULL && seq_delta(route->sample_seq_no, proxy->ack_seq_no) < 0) {",
        "        if (false) {",
        "test_best_effort_fragment_of_a_passed_sample_claims_nothing",
    ),
    (
        "an untracked BEST_EFFORT Subscriber's continuation",
        "            } else if (best_effort && frag_sub_holds(node, sub, header->source, entity_id, sample_seq_no)) {",
        "            } else if (false) {",
        "test_one_byte_more_is_two_fragments",
    ),
    (
        "a sample too large for the own slot moves",
        "    if (sub != NULL && slot == &sub->frag_own && slot->frag_count == count && !frag_fits(slot, index, length)) {",
        "    if (false) {",
        "test_sample_larger_than_its_own_slot_goes_to_the_pool",
    ),
    (
        "no-buffer RELIABLE records ahead only in the pool",
        "        if (slot == &sub->frag_own || frag_sub_spanning(node, sub, source, entity_id, proxy->ack_seq_no) == NULL ||",
        "        if (frag_sub_spanning(node, sub, source, entity_id, proxy->ack_seq_no) == NULL ||",
        "test_reliable_none_ahead_never_pins_the_own_slot",
    ),
    (
        "a destroyed Subscriber's slots go with it",
        "    frag_release_subscriber(node, sub);\n",
        "",
        "test_destroyed_subscriber_leaves_no_pool_slot",
    ),
    (
        "storage: a buffered RELIABLE Subscriber needs no slot",
        "    if (actual->reliability.kind == tt_QOS_RELIABILITY_RELIABLE && slots != 0 && slots != tt_QOS_REORDER_NONE) {",
        "    if (false) {",
        "test_own_slot_storage_size",
    ),
    (
        "storage: a type no link fragments needs none",
        "    return whole <= FRAG_NEVER_CONTROL ? 0U : cdr + 4U;",
        "    return cdr + 4U;",
        "test_own_slot_storage_size",
    ),
]


def build_and_run(control=None):
    build = run_capped(["make", "-s", "-j4", str(BIN.relative_to(ROOT / "platform/linux"))], timeout=900,
                       cwd=ROOT / "platform/linux")
    if build.returncode != 0:
        return "BUILD_FAILED", build.output[-2000:]
    if unchanged(control, BIN):
        return NOT_COMPILED, ""
    # The whole binary takes a few seconds. A mutant that loops forever is killed at the deadline and says so; one
    # that prints forever has its output dropped past mutant_guard.CAP_BYTES rather than held.
    run = run_capped([str(BIN)], timeout=180)
    if run.timed_out:
        return "HANG", run.output[-1000:]
    if run.returncode is None or run.returncode < 0:
        return "CRASH", run.output[-2000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.output


def failing_lines(output):
    return [line for line in output.splitlines() if "test_data_frag.c:" in line][:3]


def main():
    be_nice()
    check_anchors(ROOT, [(name, SRC, old, new) for name, old, new, _test in MUTANTS])
    failures = 0
    with SourceGuard(ROOT, [SRC]) as guard:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                                   -> {verdict}", flush=True)
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1
        control = code_digest(BIN)

        only = os.environ.get("MUTANTS", "")
        original = guard.original(SRC)
        for name, old, new, test in MUTANTS:
            if only and only not in name:
                continue
            (ROOT / SRC).write_text(original.replace(old, new, 1))
            try:
                verdict, output = build_and_run(control)
            finally:
                guard.restore()
            ok = verdict in ("FAIL", "CRASH", "HANG")
            failures += 0 if ok else 1
            print(f"[{label(verdict, ok):8s}] {name:52s} -> {verdict} (guard: {test})", flush=True)
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
