#!/usr/bin/env python3
"""Mutant sweep for SHM stage 1's segment-health tests.

Each entry removes exactly one mechanism and names the test that must go red for it. A mutant that
still passes means the test is not testing what its name says - which is the failure this sweep
exists to catch, and which has happened twice already in this file's history (a contention test that
passed against the pre-fix writer, and an address test that compared zero with zero).

Run from the repository root. Restores every file it touches, whatever happens.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_transport_seam"

MUTANTS = [
    (
        "negative attach caching",
        "src/tickle.c",
        "        note_attach(node, verdict);\n        remember_absent(entry, ip, port);\n",
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
        "        if (now - entry->last_progress_ns >= tt_SEGMENT_DEAD_READER_NS) {",
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
        "datagrams[i].head_len, datagrams[i].body, datagrams[i].body_len, &reason, false)) {",
        "datagrams[i].head_len, datagrams[i].body, datagrams[i].body_len, &reason, true)) {",
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
        "            note_head_stall(node); // empty, or a head nobody is coming back for - the two look alike",
        "            // note_head_stall(node); -- mutant",
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
]


def build_and_run():
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True, cwd=ROOT / "platform/linux",
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    run = subprocess.run([str(BIN)], capture_output=True, text=True)
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    backup = tempfile.mkdtemp(prefix="shm_mutants_")
    touched = {m[1] for m in MUTANTS}
    for rel in touched:
        shutil.copy2(ROOT / rel, pathlib.Path(backup) / rel.replace("/", "_"))

    def restore():
        for rel in touched:
            # copy(), not copy2(), and then touch: copy2 preserves the ORIGINAL mtime, so a restored
            # file looks older than the object files built from the mutant while it was applied, and
            # make then considers the build up to date. The next `make test` after a sweep ran a
            # binary compiled from mutated source and reported failures that were not in the tree -
            # a check answering about something other than the thing being checked, in the harness
            # whose whole job is to prevent exactly that.
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

        for name, rel, old, new, test in MUTANTS:
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                # The substitution that matches nothing is the sweep's own version of a test that
                # cannot fail, so it is an error rather than a skip.
                print(f"[ERROR]    {name:40s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            assert path.read_text().count(old) == 0, "substitution did not apply"
            verdict, output = build_and_run()
            restore()
            expected = "FAIL"
            ok = verdict == expected
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name:40s} -> {verdict} (wanted {expected}, guard: {test})")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)

    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
