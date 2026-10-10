#!/usr/bin/env python3
"""Mutant sweep for large-message step B (DESIGN.md section 8, "Step B", pre-registration B1).

Each entry breaks exactly one rule the same-host region relies on and names the test in tests/test_large_region.c that
must go red for it - the mutants B1 names (span 1 instead of the count, the descriptor off by one chunk, reuse ignoring
the readers mask, a bit cleared on the first Subscriber's delivery, no clear on a dropped descriptor, a resend as
fragments, no generation check, no clear on liveliness removal) and a few more for conditions the tests claim to cover.
A mutant that still passes means either the test does not test what its name says, or the condition decides nothing
and should not be in the code.

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
BIN = ROOT / "platform/linux/obj/debug/tests/test_large_region"
SRC = "src/tickle.c"

MUTANTS = [
    (
        "B1.1 descriptor spans one seq_no, not the sample's count",
        "    node->tx_seq_span = record->count;\n    bool handled",
        "    node->tx_seq_span = 1;\n    bool handled",
        "test_round_trip_one_record_per_sample",
    ),
    (
        "B1.1 descriptor names the next chunk",
        "    ref->chunk = (uint16_t)(record->chunk - 1U);",
        "    ref->chunk = (uint16_t)(record->chunk % pub->large_region.chunks);",
        "test_round_trip_one_record_per_sample",
    ),
    (
        "B1.1 the reader never clears its bit",
        "    __atomic_fetch_and(word, ~large_reader_bit(node->id), __ATOMIC_RELEASE); // reads of it happen before this",
        "    (void)word;",
        "test_round_trip_one_record_per_sample",
    ),
    (
        "B1.2 a chunk reused while a reader holds it",
        "        if ((region->held & (1ULL << i)) == 0 && large_chunk_unread(region->mapping, i)) {",
        "        if ((region->held & (1ULL << i)) == 0) {",
        "test_a_retained_chunk_is_not_reused",
    ),
    (
        "B1.3 the bit cleared on the first Subscriber's delivery",
        "    node->large.ref_processing = ref.chunk;\n",
        "",
        "test_the_bit_waits_for_every_subscriber",
    ),
    (
        "B1.4 no clear of the bit when the descriptor was dropped",
        "            __atomic_fetch_and(word, ~bit, __ATOMIC_RELEASE); // nobody will read it for this descriptor",
        "            (void)word;",
        "test_a_dropped_descriptor_frees_its_bit_and_is_resent_whole",
    ),
    (
        "B1.4 a NACK answered with fragments, not the descriptor",
        "    if (record->chunk != 0 && large_ref_segment(node, target) != NULL) {\n        if (record->ref_resent",
        "    if (false) {\n        if (record->ref_resent",
        "test_a_dropped_descriptor_frees_its_bit_and_is_resent_whole",
    ),
    (
        "B1.5 no generation check",
        " ||\n        __atomic_load_n(&entry->generation, __ATOMIC_ACQUIRE) != generation) {",
        ") {",
        "test_a_stale_descriptor_is_refused",
    ),
    (
        "B1.5 no check of the reader's own bit",
        "    if ((word & large_reader_bit(node->id)) == 0 ||\n        __atomic_load_n",
        "    if (__atomic_load_n",
        "test_a_stale_descriptor_is_refused",
    ),
    (
        "B1.5 no incarnation check",
        " && header->incarnation == incarnation &&\n",
        " &&\n",
        "test_a_stale_descriptor_is_refused",
    ),
    (
        "B1.6 liveliness removal leaves the dead reader's bits",
        "    large_region_forget_reader(node, source); // what it held in this context's regions is free again (step B)\n",
        "",
        "test_a_dead_readers_chunk_is_freed_by_liveliness_not_before",
    ),
    (
        "B1.7 the owner's feature bit ignored",
        "    return segment != NULL && (segment->features & tt_SEGMENT_FEATURE_LARGE_REF) != 0 ? segment : NULL;",
        "    return segment;",
        "test_a_reader_without_the_feature_gets_fragments",
    ),
    (
        "B1.7 a descriptor from a socket taken",
        "        if (transport != tt_TRANSPORT_SHM) {\n            // It names memory",
        "        if (false) {\n            // It names memory",
        "test_a_descriptor_from_a_socket_is_refused",
    ),
    (
        "B1.8 no fallback when the region is exhausted",
        "    return (uint8_t*)node->large.acquire(node->large.user, bytes);\n}",
        "    return NULL;\n}",
        "test_an_exhausted_region_falls_back_to_fragments",
    ),
    (
        "B1.8 a sample above the chunk taken into it",
        "    if (bytes > region->chunk_bytes || (region->mapping == NULL && !large_region_create(node, pub))) {",
        "    if (region->mapping == NULL && !large_region_create(node, pub)) {",
        "test_a_sample_above_the_chunk_goes_as_fragments",
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
    return [line for line in output.splitlines() if "test_large_region.c:" in line][:3]


def main():
    backup = tempfile.mkdtemp(prefix="large_region_mutants_")
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
