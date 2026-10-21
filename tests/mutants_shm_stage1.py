#!/usr/bin/env python3
"""Mutant sweep for SHM stage 1's segment-health tests.

Each entry removes exactly one mechanism and names the test that must go red for it. A mutant that
still passes means the test is not testing what its name says - which is the failure this sweep
exists to catch, and which has happened twice already in this file's history (a contention test that
passed against the pre-fix writer, and an address test that compared zero with zero).

Run from the repository root. MUTANTS=<substring> runs only the mutants whose name contains it (the control always
runs).

Refuses to start (exit 2) if any anchor is not in its file exactly once, checked for every mutant before the control
runs - from f3c2f6d9 to 2026-10-21 "ringing for a batch once" matched twice and "stalled-head detection" not at all,
and the sweep reported them as two problems among its verdicts rather than refusing - or if a file it edits is already
modified. Every child is bounded (tests/mutant_guard.py: 2 GB of address space, a deadline, at most 256 KB of its
output kept); the files are restored after each mutant, on an exception and on SIGINT/SIGTERM/SIGHUP, with a fresh
mtime so make rebuilds.
"""
import os
import pathlib
import sys

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged
from mutant_guard import SourceGuard, be_nice, check_anchors, killed_by, run_capped, usage_line

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_transport_seam"

MUTANTS = [
    (
        "negative attach caching",
        "src/tickle.c",
        "        note_attach(node, verdict);\n        remember_absent(node, entry, ip, port);\n",
        "        note_attach(node, verdict);\n",
        "test_a_peer_with_no_segment_is_asked_once_not_per_datagram",
    ),
    (
        "revalidation of an attached segment",
        "src/tickle.c",
        "        if (same_owner && entry->recheck_in > 0) {",
        "        if (same_owner) {",
        "test_a_segment_left_by_a_dead_owner_is_reclaimed",
    ),
    (
        "a successful write restarting the dead-reader clock",
        "src/tickle.c",
        "    segment_peer(node, context_id)->last_progress_ns = 0;",
        "    // mutant: leave the dead-reader clock running through a success",
        "test_a_writer_gives_up_on_a_ring_nobody_drains",
    ),
    (
        "giving up on a ring nobody drains",
        "src/tickle.c",
        "        if (now - entry->last_progress_ns >= node->opts.segment.dead_reader_ns && entry->claims == 0) {",
        "        if (false) {",
        "test_a_writer_gives_up_on_a_ring_nobody_drains",
    ),
    (
        "ringing the doorbell for a sleeping reader",
        "src/tickle.c",
        "    if (sleeping != 0) {",
        "    if (false) {",
        "test_a_sleeping_reader_is_rung_and_a_busy_one_is_not",
    ),
    (
        "ringing only once for a reader that never answers",
        "src/tickle.c",
        "        if (sleeping != peer->doorbell_generation) {",
        "        if (true) {",
        "test_a_sleeping_reader_is_rung_and_a_busy_one_is_not",
    ),
    (
        "ringing for a batch once, after its last record",
        "src/tickle.c",
        # seam_send_batch() has two bodies since f3c2f6d9: the tt_HAL_SEND_BATCH_LOOPS one (FreeRTOS, not compiled
        # into this test) and the sendmmsg() one Linux builds. The anchor takes the latter by its own comment; the
        # former cannot decide anything here and would only come back NOT_COMPILED.
        "    // The peers this batch wrote to, each rung once after the whole batch is in (segment_ring_if_asleep()).\n"
        "    uint8_t rung_for[TX_MAX_BATCH];\n"
        "    uint32_t peers = 0;\n"
        "    for (uint32_t i = 0; i < count; i++) {\n"
        "        enum udp_reason reason = UDP_BECAUSE_UNATTACHED;\n"
        "        if (segment_deliver_ringing(node, context_ids[i], datagrams[i].ip, datagrams[i].port, datagrams[i].head,\n"
        "                                    datagrams[i].head_len, datagrams[i].body, datagrams[i].body_len, &reason, false)) {",
        "    // The peers this batch wrote to, each rung once after the whole batch is in (segment_ring_if_asleep()).\n"
        "    uint8_t rung_for[TX_MAX_BATCH];\n"
        "    uint32_t peers = 0;\n"
        "    for (uint32_t i = 0; i < count; i++) {\n"
        "        enum udp_reason reason = UDP_BECAUSE_UNATTACHED;\n"
        "        if (segment_deliver_ringing(node, context_ids[i], datagrams[i].ip, datagrams[i].port, datagrams[i].head,\n"
        "                                    datagrams[i].head_len, datagrams[i].body, datagrams[i].body_len, &reason, true)) {",
        "test_a_batch_is_rung_for_once_after_its_last_record",
    ),
    (
        "not ringing it for a reader that is awake",
        "src/tickle.c",
        "    if (sleeping != 0) {",
        "    if (true) {",
        "test_a_sleeping_reader_is_rung_and_a_busy_one_is_not",
    ),
    (
        "stalled-head detection",
        "src/tickle.c",
        # b3bcfb62 (a reference read at the head counts as a record taken) made note_head_stall() return an enum head_stall
        # instead of a bool; the drain's one call to it is still the mechanism. The mutant never asks it and takes the
        # drain as run dry, as the old one did. (void) keeps the now-uncalled static function referenced.
        "                const enum head_stall stall = note_head_stall(node);\n"
        "                ran_dry = ((unsigned)stall & 1U) == 0;",
        "                const enum head_stall stall = HEAD_STALL_STOP; // note_head_stall(node) not asked -- mutant\n"
        "                (void)note_head_stall;\n"
        "                ran_dry = ((unsigned)stall & 1U) == 0;",
        "test_a_stalled_head_is_noticed_rather_than_read_as_a_sizing_problem",
    ),
    (
        "warn-once latch on a full ring",
        "src/tickle.c",
        "        if (node->segment_full_warnings == 0) {\n            node->segment_full_warnings++;",
        "        if (true) {\n            node->segment_full_warnings++;",
        "test_capacity_exhaustion_is_counted_and_warned_once",
    ),
    (
        "the empty ring costing no lock",
        "src/tickle.c",
        "    struct tt_SegmentHeader* header = node->own_segment;\n    if (__atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE) ==",
        "    struct tt_SegmentHeader* header = node->own_segment;\n    if (false && __atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE) ==",
        "test_the_drain_empties_the_ring_or_says_it_did_not",
    ),
    (
        "draining the ring past the old 64-record cap",
        "include/tickle/config.h",
        "#define tt_SEGMENT_DRAIN_PER_POLL (tt_SEGMENT_SLOTS * 4)",
        "#define tt_SEGMENT_DRAIN_PER_POLL 64",
        "test_the_drain_empties_the_ring_or_says_it_did_not",
    ),
    (
        "dropping rather than rerouting past a full ring",
        "src/tickle.c",
        "        return true; // handled: by dropping it, which is the ordered thing to do",
        "        *reason = UDP_BECAUSE_UNATTACHED;\n        return false; // mutant: reroute it",
        "test_capacity_exhaustion_is_counted_and_warned_once",
    ),
    (
        "resetting the segment state a caller did not zero",
        "src/tickle.c",
        "    memset(node->segment_peer_live, 0, sizeof(node->segment_peer_live));",
        "    // mutant: leave segment_peer_live as the caller left it",
        "test_reset_zeroes_the_per_transport_counters",
    ),
    # Stage 1 / S1 (2026-10-09): the module on and unused must not write the 14 KB peer table at create. The mutant is
    # the code before the change - the whole table zeroed at reset - so the test fails on exactly what was fixed.
    (
        "leaving the peer table unwritten until an entry is wanted",
        "src/tickle.c",
        "    memset(node->segment_peer_live, 0, sizeof(node->segment_peer_live));",
        "    memset(node->segment_peers, 0, sizeof(node->segment_peers)); // mutant: the table zeroed at reset\n"
        "    memset(node->segment_peer_live, 0, tt_MAX_CONTEXT_IDS);",
        "test_a_context_whose_peers_are_all_remote_leaves_the_peer_table_unwritten",
    ),
    (
        "unmapping the own segment exactly once at teardown",
        "src/tickle.c",
        "        if (entry->mapping != NULL && entry->mapping != own) {",
        "        if (entry->mapping != NULL) {",
        "test_destroy_takes_the_segment_with_it",
    ),
    (
        "segment teardown at context destroy",
        "src/tickle.c",
        "    release_segments(node);",
        "    // release_segments(node); -- mutant",
        "test_destroy_takes_the_segment_with_it",
    ),
    # Lazy segment creation (stage 1, option B). The claim here is mostly a NEGATIVE one - that no
    # segment is built until something can use it - and a test of an absence passes just as well
    # against the old eager code unless the controls below can fail.
    (
        "the address deciding whether a peer is on this host",
        "src/tickle.c",
        "    if (own_ip == 0 || peer_ip != own_ip) {",
        "    if (own_ip == 0) {",
        "test_a_peer_on_another_host_builds_nothing",
    ),
    (
        "the discovery edge building the segment at all",
        "src/tickle.c",
        "        node->same_host_peer_count++;\n    }\n    ensure_own_segment(node);",
        "        node->same_host_peer_count++;\n    }",
        "test_a_same_host_peer_appearing_builds_the_segment",
    ),
    (
        "self-delivery building the segment it needs",
        "src/tickle.c",
        "    if (context_id == node->id) {",
        "    if (false && context_id == node->id) {",
        "test_delivering_to_ourselves_builds_the_segment",
    ),
    (
        "releasing only on the LAST departure",
        "src/tickle.c",
        "    if (node->same_host_peer_count == 0 && !own_segment_attached_by_self(node)) {",
        "    if (!own_segment_attached_by_self(node)) {",
        "test_the_last_same_host_peer_leaving_takes_the_segment",
    ),
    (
        "ignoring a departure for a peer never counted",
        "src/tickle.c",
        "    if (context_id == tt_CONTEXT_ID_INVALID || !node->same_host_peer[context_id]) {",
        "    if (context_id == tt_CONTEXT_ID_INVALID) {",
        "test_a_peer_we_never_counted_leaving_changes_nothing",
    ),
    (
        "keeping the segment a context still delivers to itself through",
        "src/tickle.c",
        "    if (node->same_host_peer_count == 0 && !own_segment_attached_by_self(node)) {",
        "    if (node->same_host_peer_count == 0) {",
        "test_a_context_delivering_to_itself_keeps_its_segment",
    ),
    (
        "unlinking the file, not merely unmapping it",
        "src/tickle.c",
        "    if (named) {\n        tt_segment_unlink(path);\n    }\n    release_own_bell(node, own);",
        "    (void)named;\n    release_own_bell(node, own);",
        "test_the_last_same_host_peer_leaving_takes_the_segment",
    ),
    (
        "finding our own attachment by identity rather than at index node->id",
        "src/tickle.c",
        "    if (node->same_host_peer_count == 0 && !own_segment_attached_by_self(node)) {",
        "    if (node->same_host_peer_count == 0 && node->segment_peers[node->id].mapping == NULL) {",
        "test_a_renumbered_context_still_knows_it_holds_its_own_segment",
    ),
    # The geometry check (2026-10-02). Measured before it was written: a 512-slot owner against a
    # 256-slot attacher does NOT fault - it wedges the ring and drops 1535 of 2000 messages silently,
    # while the publisher reports success and exits 0. The opposite direction was already refused by
    # tt_segment_attach()'s fstat, which is why only half of this was ever covered.
    (
        "refusing a geometry that cannot be indexed",
        "src/tickle.c",
        "    if (header->slots == 0 || (header->slots & (header->slots - 1U)) != 0 || header->slot_bytes == 0) {",
        "    if (false) {",
        "test_a_segment_with_another_geometry_is_refused",
    ),
    (
        "checking slot_bytes as well as slots",
        "src/tickle.c",
        "    if (header->slots == 0 || (header->slots & (header->slots - 1U)) != 0 || header->slot_bytes == 0) {",
        "    if (header->slots == 0 || (header->slots & (header->slots - 1U)) != 0) {",
        "test_a_segment_with_another_geometry_is_refused",
    ),
    # 324f6ced's claim pin met 97893bb1's lazy peer table: read raw, an entry never set up held a garbage pin and the
    # attach was skipped for good in any context whose memory nobody had zeroed.
    (
        "reading the claim pin only through the set-up guard",
        "src/tickle.c",
        "    const struct tt_SegmentPeer* entry = segment_peer_if_live(node, context_id);\n"
        "    if (entry != NULL && entry->claims != 0) {",
        "    const struct tt_SegmentPeer* entry = &node->segment_peers[context_id];\n    if (entry->claims != 0) {",
        "test_a_context_in_unzeroed_memory_attaches_a_same_host_peer",
    ),
]


def build_and_run(control=None):
    linux = ROOT / "platform/linux"
    build = run_capped(["make", "-s", "-j2", str(BIN.relative_to(linux))], timeout=900, cwd=linux)
    if build.returncode != 0:
        return "BUILD_FAILED", build.output[-2000:]
    if unchanged(control, BIN):
        return NOT_COMPILED, ""
    run = run_capped([str(BIN)], timeout=300, cwd=linux)
    if run.timed_out:
        return "HANG", run.output  # a kill too: the test did not pass
    return ("PASS" if run.returncode == 0 else "FAIL"), run.output


def main():
    be_nice()
    check_anchors(ROOT, [(name, rel, old, new) for name, rel, old, new, _test in MUTANTS])
    failures = 0
    with SourceGuard(ROOT, [m[1] for m in MUTANTS]) as guard:
        verdict, output = build_and_run()
        print(f"[control]  parent, unmutated                      -> {verdict}", flush=True)
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 2
        control = code_digest(BIN)

        only = os.environ.get("MUTANTS", "")
        for name, rel, old, new, test in MUTANTS:
            if only and only not in name:
                continue
            path = ROOT / rel
            path.write_text(guard.original(rel).replace(old, new, 1))
            try:
                verdict, output = build_and_run(control)
            finally:
                guard.restore()
            ok = verdict in ("FAIL", "HANG")
            failures += 0 if ok else 1
            print(f"[{label(verdict, ok):8s}] {name:40s} -> {verdict} (wanted FAIL, guard: {test})", flush=True)
            if ok:
                print(f"           killed by: {', '.join(killed_by(output, ROOT)) or '(no failing check printed)'}")
            else:
                print(output[-1500:])

    print(usage_line())
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
