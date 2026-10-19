#!/usr/bin/env python3
"""Mutant sweep for the RELIABLE in-order fragment fast path (frag_fast_take(), tickle.c, 2026-10-06).

Each entry removes exactly one condition or mechanism of the fast path and names the test in
tests/test_data_frag.c that must go red for it. A mutant that still passes means either the test does not
test what its name says, or the condition decides nothing and should not be in the code.

Not swept: frag_fast_spill()'s "slot already occupied" branch. It is unreachable by construction - the
Subscriber held nothing when its sample started, and every store into its buffer moves the sample out
first - and the two collision tests below are what show that construction holds.

Run from the repository root: python3 tests/mutants_frag_fast_path.py. Refuses to start if any anchor is not in
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
        "in order: seq_no is the watermark",
        "    if (ctx->seq_no != proxy->ack_seq_no || reorder_payload_capacity(sub) < length) {",
        "    if (reorder_payload_capacity(sub) < length) {",
        "test_reliable_fast_path_does_not_overtake_a_lost_sample",
    ),
    (
        "a reorder slot could hold the fragment",
        "    if (ctx->seq_no != proxy->ack_seq_no || reorder_payload_capacity(sub) < length) {",
        "    if (ctx->seq_no != proxy->ack_seq_no) {",
        "test_reliable_fast_path_needs_room_in_the_buffer",
    ),
    (
        "the scratch is free",
        "        if (node->frag_fast_sub != NULL || sub->reorder_held != 0) {",
        "        if (sub->reorder_held != 0) {",
        "test_reliable_two_writers_interleaved_after_first_contact",
    ),
    (
        "the Subscriber holds nothing",
        "        if (node->frag_fast_sub != NULL || sub->reorder_held != 0) {",
        "        if (node->frag_fast_sub != NULL) {",
        "test_reliable_fast_path_waits_while_anything_is_held",
    ),
    (
        "a continuation of the scratch's own sample",
        "    } else if (!frag_fast_holds(node, sub, proxy) || index != node->frag_fast_placed ||",
        "    } else if (index != node->frag_fast_placed ||",
        "test_reliable_two_subscribers_share_the_scratch",
    ),
    (
        "the next index",
        "    } else if (!frag_fast_holds(node, sub, proxy) || index != node->frag_fast_placed ||",
        "    } else if (!frag_fast_holds(node, sub, proxy) ||",
        "test_reliable_fast_path_refuses_what_does_not_belong_to_its_sample",
    ),
    (
        "the same frag_count",
        "               ctx->frag_count != node->frag_fast_count ||\n",
        "               false ||\n",
        "test_reliable_fast_path_refuses_what_does_not_belong_to_its_sample",
    ),
    (
        "continuations of one size",
        "(index > 1 && index + 1U != node->frag_fast_count && length != node->frag_fast_cont_length)",
        "false",
        "test_reliable_fast_path_refuses_what_does_not_belong_to_its_sample",
    ),
    (
        "  ... but not the first continuation",
        "(index > 1 && index + 1U != node->frag_fast_count && length != node->frag_fast_cont_length)",
        "(index + 1U != node->frag_fast_count && length != node->frag_fast_cont_length)",
        "test_reliable_in_order_sample_never_touches_the_reorder_buffer",
    ),
    (
        "  ... and not the last",
        "(index > 1 && index + 1U != node->frag_fast_count && length != node->frag_fast_cont_length)",
        "(index > 1 && length != node->frag_fast_cont_length)",
        "test_reliable_in_order_sample_never_touches_the_reorder_buffer",
    ),
    (
        "the sample fits the scratch",
        "    if (node->frag_fast_length + length > sizeof(node->frag_scratch) - 8) {",
        "    if (false) {",
        # Not the refusal test: there the overflowing copy wrote garbage over frag_fast_count and frag_fast_placed,
        # and the next checks refused the sample for it - nothing delivered, as expected, so the mutant survived.
        "test_reliable_fast_path_writes_nothing_past_its_scratch",
    ),
    (
        "move out before a DATA is held",
        "        frag_fast_spill(node); // this buffer is about to hold something, and it must be empty for that\n",
        "",
        "test_reliable_fast_path_moves_out_before_another_writer_is_held",
    ),
    (
        "  ... only for its own Subscriber",
        "    if (frag_fast_holds(node, sub, NULL)) {\n"
        "        frag_fast_spill(node); // this buffer is about to hold something",
        "    if (node->frag_fast_sub != NULL) {\n"
        "        frag_fast_spill(node); // this buffer is about to hold something",
        "test_reliable_fast_path_ignores_another_subscribers_hold",
    ),
    (
        "move out before a fragment is stored",
        "        frag_fast_spill(node); // the fast path's sample needs this buffer empty to be moved into it\n",
        "",
        "test_reliable_fast_path_interrupted_by_a_lost_fragment",
    ),
    (
        "  ... only for its own Subscriber",
        "    if (frag_fast_holds(node, sub, NULL)) {\n"
        "        frag_fast_spill(node); // the fast path's sample",
        "    if (node->frag_fast_sub != NULL) {\n"
        "        frag_fast_spill(node); // the fast path's sample",
        "test_reliable_two_subscribers_share_the_scratch",
    ),
    (
        "move out before the scratch is reused",
        "    frag_fast_spill(node); // the scratch is about to be overwritten; whatever it holds goes to its own buffer\n",
        "",
        "test_reliable_buffer_delivery_does_not_overwrite_the_fast_path",
    ),
    (
        "move out when the watermark moves",
        "    if (frag_fast_holds(node, sub, proxy) && proxy->ack_seq_no != node->frag_fast_seq_no + node->frag_fast_placed) {",
        "    if (false) {",
        "test_reliable_fast_path_sample_given_up_on_is_never_delivered_torn",
    ),
    (
        "  ... only when it moves",
        "    if (frag_fast_holds(node, sub, proxy) && proxy->ack_seq_no != node->frag_fast_seq_no + node->frag_fast_placed) {",
        "    if (frag_fast_holds(node, sub, proxy)) {",
        "test_reliable_fast_path_survives_another_writers_drain",
    ),
    (
        "  ... only for its own writer",
        "    if (frag_fast_holds(node, sub, proxy) && proxy->ack_seq_no != node->frag_fast_seq_no + node->frag_fast_placed) {",
        "    if (node->frag_fast_sub != NULL && proxy->ack_seq_no != node->frag_fast_seq_no + node->frag_fast_placed) {",
        "test_reliable_fast_path_survives_another_writers_drain",
    ),
    (
        "moving out rewinds the reorder_cursor",
        "        proxy->reorder_cursor = node->frag_fast_seq_no;\n",
        "",
        "test_reliable_fast_path_interrupted_by_a_lost_fragment",
    ),
    (
        "the cursor follows a fast delivery",
        "    proxy->reorder_cursor = proxy->ack_seq_no;\n",
        "",
        "test_reliable_in_order_sample_never_touches_the_reorder_buffer",
    ),
    (
        "a forgotten writer takes its sample",
        "        node->frag_fast_sub = NULL; // the fast path's part of a sample, abandoned like a held one\n",
        "",
        "test_reliable_fast_path_follows_its_writer_and_subscriber_out",
    ),
    (
        "  ... only that writer's node",
        "    if (node->frag_fast_sub == sub && node->frag_fast_context_id == node_id &&",
        "    if (node->frag_fast_sub == sub &&",
        "test_reliable_fast_path_follows_its_writer_and_subscriber_out",
    ),
    (
        "  ... only that entity",
        "        (match_any_entity || node->frag_fast_entity_id == entity_id)) {",
        "        (true)) {",
        "test_reliable_fast_path_follows_its_writer_and_subscriber_out",
    ),
    (
        "  ... or every entity of the node",
        "        (match_any_entity || node->frag_fast_entity_id == entity_id)) {",
        "        (node->frag_fast_entity_id == entity_id)) {",
        "test_reliable_fast_path_follows_its_writer_and_subscriber_out",
    ),
    (
        "a destroyed Subscriber leaves no pointer",
        "        node->frag_fast_sub = NULL; // its part-assembled sample goes with it, as its reorder buffer does\n",
        "",
        "test_reliable_fast_path_follows_its_writer_and_subscriber_out",
    ),
    (
        "the fast path itself",
        "        if (frag_fast_take(node, sub, proxy, ctx, is_native)) {\n            return;\n        }\n",
        "",
        "test_reliable_in_order_sample_never_touches_the_reorder_buffer",
    ),
]


def build_and_run(control=None):
    build = run_capped(["make", "-s", "-j4", str(BIN.relative_to(ROOT / "platform/linux"))], timeout=900,
                       cwd=ROOT / "platform/linux")
    if build.returncode != 0:
        return "BUILD_FAILED", build.output[-2000:]
    if unchanged(control, BIN):
        return NOT_COMPILED, ""
    # The whole binary takes about two seconds. A mutant that loops forever is killed at the deadline and says so; one
    # that prints forever has its output dropped past mutant_guard.CAP_BYTES rather than held (2026-10-11).
    run = run_capped([str(BIN)], timeout=120)
    if run.timed_out:
        return "HANG", run.output[-1000:]
    # A crash is a kill too (a mutant writing past frag_scratch may well crash), but say which it was.
    if run.returncode is None or run.returncode < 0:
        return "CRASH", run.output[-2000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.output


def failing_lines(output):
    return [line for line in output.splitlines() if "test_data_frag.c:" in line][:3]


def main():
    be_nice()
    check_anchors(ROOT, [(name, SRC, old, new) for name, old, new, _test in MUTANTS])
    failures = 0
    # Restores tickle.c after each mutant, in a finally and on SIGINT/SIGTERM/SIGHUP, with a fresh mtime: copy2() kept
    # the old one and make reused a binary built from the mutant (tests/mutants_shm_stage1.py, 2026-09-29).
    with SourceGuard(ROOT, [SRC]) as guard:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                           -> {verdict}", flush=True)
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
            print(f"[{label(verdict, ok):8s}] {name:45s} -> {verdict} (guard: {test})", flush=True)
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
