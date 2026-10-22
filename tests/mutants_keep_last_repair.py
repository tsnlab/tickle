#!/usr/bin/env python3
"""Mutant sweep for RELIABLE repair as DDS/RTPS has it (DESIGN.md section 6, 2026-10-15).

A writer resends a datagram for as long as its history holds it and says when it no longer does (the eviction
HEARTBEAT); a reader gives a gap up only on that word or on the writer's departure, and backs its requests off past
tt_RELIABLE_RETRY. Each entry puts back, or breaks, exactly one condition of that and names the test binary that must
go red. A mutant that still passes means the test does not test what it says, or the condition decides nothing.

Run from the repository root. Restores every file it touches, whatever happens. MUTANTS=<substring> runs only the
mutants whose name contains it (the controls always run). Each test run is capped at 2 GB of address space, 300 s
and its output goes to a file, of which only the tail is read.
"""
import os
import pathlib
import resource
import shutil
import signal
import subprocess
import sys
import tempfile

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged

ROOT = pathlib.Path(__file__).resolve().parents[1]
TESTS = ROOT / "platform/linux/obj/debug/tests"
PUBSUB = "test_reliable_pubsub"
SEND = "test_hybrid_send"
DIFF = "test_ack_bitmap_diff"

# (name, file, old, new, test binary that must fail)
MUTANTS = [
    (
        "reader, repair_timing: ignored (the default rule on a WIFI link)",
        "src/tickle.c",
        "    bool path = __builtin_expect(node->repair_timing, 0) ? repair_timing_times(proxy) : plain;",
        "    bool path = plain;",
        PUBSUB,
    ),
    (
        "reader, repair_timing: no seed past the back-off",
        "src/tickle.c",
        "    return proxy->retry == 0 || proxy->recovery_srtt_ns == 0;",
        "    return proxy->retry == 0;",
        PUBSUB,
    ),
    (
        "reader, repair_timing: retried recoveries timed after the seed (no Karn)",
        "src/tickle.c",
        "    return proxy->retry == 0 || proxy->recovery_srtt_ns == 0;",
        "    return proxy->retry <= 3U || proxy->recovery_srtt_ns == 0;",
        PUBSUB,
    ),
    (
        "reader, repair_timing: only the seed timed",
        "src/tickle.c",
        "    return proxy->retry == 0 || proxy->recovery_srtt_ns == 0;",
        "    return proxy->recovery_srtt_ns == 0;",
        PUBSUB,
    ),
    (
        "reader, repair_timing: a recovery the rule leaves out still timed when plain",
        "src/tickle.c",
        "        if (path && !proxy->probe_busy) {",
        "        if (!proxy->probe_busy) {",
        PUBSUB,
    ),
    (
        "reader, repair_timing: a busy recovery seeds",
        "src/tickle.c",
        "        if (path && !proxy->probe_busy) {",
        "        if (path) {",
        PUBSUB,
    ),
    (
        "reader, repair_timing: a seed past the back-off never reached (plain gate only)",
        "src/tickle.c",
        "    if (node->rx_targeted && (plain || path)) {",
        "    if (node->rx_targeted && plain) {",
        PUBSUB,
    ),
    (
        "writer: the per-sample resend cap put back",
        "src/tickle.c",
        "    // Same \"as if it had never been sent\" rule deliver_durability_backlog() applies, here for a",
        "    if (cache_entry->retry >= tt_RELIABLE_RETRY) {\n        return NULL;\n    }\n"
        "    // Same \"as if it had never been sent\" rule deliver_durability_backlog() applies, here for a",
        PUBSUB,
    ),
    (
        "writer: the per-sample resend cap put back (end to end)",
        "src/tickle.c",
        "    // Same \"as if it had never been sent\" rule deliver_durability_backlog() applies, here for a",
        "    if (cache_entry->retry >= tt_RELIABLE_RETRY) {\n        return NULL;\n    }\n"
        "    // Same \"as if it had never been sent\" rule deliver_durability_backlog() applies, here for a",
        SEND,
    ),
    (
        "writer: no resend suppression past tt_RELIABLE_RETRY",
        "src/tickle.c",
        "    if (resend_suppressed(node, cache_entry, pub_keep_all)) {",
        "    if (false && resend_suppressed(node, cache_entry, pub_keep_all)) {",
        PUBSUB,
    ),
    (
        "writer: suppression from the first resend",
        "src/tickle.c",
        "    if (pub_keep_all || entry->retry < node->opts.reliable.retry) {",
        "    if (pub_keep_all) {",
        PUBSUB,
    ),
    (
        "writer: KEEP_ALL suppressed too",
        "src/tickle.c",
        "    if (pub_keep_all || entry->retry < node->opts.reliable.retry) {",
        "    if (entry->retry < node->opts.reliable.retry) {",
        PUBSUB,
    ),
    (
        "writer: suppression that never ends",
        "src/tickle.c",
        "    return now - entry->resent_at < (uint32_t)(reliable_retry_interval_publisher(node) >> RESENT_AT_SHIFT);",
        "    return true;",
        PUBSUB,
    ),
    (
        "writer: suppression that never ends (end to end)",
        "src/tickle.c",
        "    return now - entry->resent_at < (uint32_t)(reliable_retry_interval_publisher(node) >> RESENT_AT_SHIFT);",
        "    return true;",
        SEND,
    ),
    (
        "writer: the resend time never recorded",
        "src/tickle.c",
        "        cache_entry->resent_at = (uint32_t)(tt_get_ns() >> RESENT_AT_SHIFT);",
        "",
        PUBSUB,
    ),
    (
        "reader: the KEEP_LAST give-up after tt_RELIABLE_RETRY put back",
        "src/tickle.c",
        "    RSTAT_INC(acknack_timer);\n#if tt_LARGE_SAMPLES\n",
        "    if (proxy->keep_all == tt_WRITER_KEEP_ALL_NO && proxy->retry > tt_RELIABLE_RETRY) {\n"
        "        proxy->sub->gap_abandoned++;\n        proxy->acknack_scheduled = false;\n"
        "        advance_ack_seq_no(proxy);\n        drain_reorder(node, proxy->sub, proxy);\n"
        "        maybe_arm_acknack_retry(node, proxy);\n        return;\n    }\n"
        "    RSTAT_INC(acknack_timer);\n#if tt_LARGE_SAMPLES\n",
        PUBSUB,
    ),
    (
        "reader: the KEEP_LAST give-up put back (end to end)",
        "src/tickle.c",
        "    RSTAT_INC(acknack_timer);\n#if tt_LARGE_SAMPLES\n",
        "    if (proxy->keep_all == tt_WRITER_KEEP_ALL_NO && proxy->retry > tt_RELIABLE_RETRY) {\n"
        "        proxy->sub->gap_abandoned++;\n        proxy->acknack_scheduled = false;\n"
        "        advance_ack_seq_no(proxy);\n        drain_reorder(node, proxy->sub, proxy);\n"
        "        maybe_arm_acknack_retry(node, proxy);\n        return;\n    }\n"
        "    RSTAT_INC(acknack_timer);\n#if tt_LARGE_SAMPLES\n",
        SEND,
    ),
    (
        "reader: the KEEP_LAST give-up put back (bitmap differential's control)",
        "src/tickle.c",
        "    RSTAT_INC(acknack_timer);\n#if tt_LARGE_SAMPLES\n",
        "    if (proxy->keep_all == tt_WRITER_KEEP_ALL_NO && proxy->retry > tt_RELIABLE_RETRY) {\n"
        "        proxy->sub->gap_abandoned++;\n        proxy->acknack_scheduled = false;\n"
        "        advance_ack_seq_no(proxy);\n        drain_reorder(node, proxy->sub, proxy);\n"
        "        maybe_arm_acknack_retry(node, proxy);\n        return;\n    }\n"
        "    RSTAT_INC(acknack_timer);\n#if tt_LARGE_SAMPLES\n",
        DIFF,
    ),
    (
        "back-off: none",
        "src/tickle.c",
        "    return interval << proxy->backoff;",
        "    return interval;",
        PUBSUB,
    ),
    (
        "back-off: KEEP_ALL backs off too",
        "src/tickle.c",
        "    if (proxy->keep_all == tt_WRITER_KEEP_ALL_YES) {\n        return interval;",
        "    if (false) {\n        return interval;",
        PUBSUB,
    ),
    (
        "back-off: doubles one retry early",
        "src/tickle.c",
        "    if (frontier != proxy->retry_frontier || proxy->retry <= reliable->retry) {",
        "    if (frontier != proxy->retry_frontier || proxy->retry < reliable->retry) {",
        PUBSUB,
    ),
    (
        "back-off: no ceiling",
        "src/tickle.c",
        "    } else if (proxy->backoff < shift) {",
        "    } else {",
        PUBSUB,
    ),
    (
        "back-off: also while the writer's DATA arrives",
        "src/tickle.c",
        "    if (frontier != proxy->retry_frontier || proxy->retry <= reliable->retry) {",
        "    if (proxy->retry <= reliable->retry) {",
        PUBSUB,
    ),
    (
        "back-off: a new gap starts where the last one's back-off ended",
        "src/tickle.c",
        "    if (frontier != proxy->retry_frontier || proxy->retry <= reliable->retry) {",
        "    if (frontier != proxy->retry_frontier) {",
        PUBSUB,
    ),
    (
        "back-off: the frontier never recorded",
        "src/tickle.c",
        "        proxy->retry_frontier = frontier;\n",
        "",
        PUBSUB,
    ),
    (
        "retry count: wraps instead of saturating",
        "src/tickle.c",
        "    bool counted = proxy->retry < UINT8_MAX;",
        "    bool counted = true;",
        PUBSUB,
    ),
    (
        "estimate: a backed-off recovery timed",
        "src/tickle.c",
        "    if (node->rx_targeted && proxy->retry <= node->opts.reliable.retry) {",
        "    if (node->rx_targeted) {",
        PUBSUB,
    ),
    (
        "writer without history: no answer",
        "src/tickle.c",
        "        if (pub->reliable && !bitmap_is_zero(bitmap, bitmap_words)) {",
        "        if (false) {",
        PUBSUB,
    ),
    (
        "writer without history: answers a pure acknowledgement too",
        "src/tickle.c",
        "        if (pub->reliable && !bitmap_is_zero(bitmap, bitmap_words)) {",
        "        if (pub->reliable) {",
        PUBSUB,
    ),
    (
        "writer without history: answers for a BEST_EFFORT Publisher too",
        "src/tickle.c",
        "        if (pub->reliable && !bitmap_is_zero(bitmap, bitmap_words)) {",
        "        if (!bitmap_is_zero(bitmap, bitmap_words)) {",
        PUBSUB,
    ),
    (
        "writer without history: says only the requested seq_no is gone",
        "src/tickle.c",
        "            encode_and_send_heartbeat(node, pub, publisher_announced_last_seq_no(pub) + 1U, &target, 1,",
        "            encode_and_send_heartbeat(node, pub, seq_no + 1U, &target, 1,",
        PUBSUB,
    ),
]


def limit_child():
    two_gb = 2 * 1024 * 1024 * 1024
    resource.setrlimit(resource.RLIMIT_AS, (two_gb, two_gb))
    resource.setrlimit(resource.RLIMIT_FSIZE, (256 * 1024 * 1024, 256 * 1024 * 1024))


def build_and_run(binary, scratch, control=None):
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), f"obj/debug/tests/{binary}"],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    if unchanged(control, TESTS / binary):
        return NOT_COMPILED, ""
    out = scratch / f"{binary}.out"
    with open(out, "wb") as sink:
        try:
            run = subprocess.run([str(TESTS / binary)], stdout=sink, stderr=subprocess.STDOUT, timeout=300,
                                 preexec_fn=limit_child)
        except subprocess.TimeoutExpired:
            return "HANG", ""
    with open(out, "rb") as source:
        source.seek(0, os.SEEK_END)
        source.seek(max(0, source.tell() - 2000))
        tail = source.read().decode(errors="replace")
    if run.returncode < 0:
        return "CRASH", tail
    return ("PASS" if run.returncode == 0 else "FAIL"), tail


def main():
    files = sorted({m[1] for m in MUTANTS})
    backup = pathlib.Path(tempfile.mkdtemp(prefix="keep_last_repair_mutants_"))
    scratch = backup / "out"
    scratch.mkdir()
    for name in files:
        (backup / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, backup / name)

    def restore():
        # copy(), then touch: copy2 would keep the original mtime, older than the mutant's objects, and make would then
        # reuse a binary built from mutated source (tests/mutants_shm_stage1.py, 2026-10-02).
        for name in files:
            shutil.copy(backup / name, ROOT / name)
            (ROOT / name).touch()

    signal.signal(signal.SIGTERM, lambda _s, _f: sys.exit(1))
    only = os.environ.get("MUTANTS", "")
    failures = 0
    digests = {}
    try:
        for binary in sorted({m[4] for m in MUTANTS}):
            verdict, output = build_and_run(binary, scratch)
            print(f"[control]  {binary:<28} unmutated -> {verdict}", flush=True)
            if verdict != "PASS":
                print(output)
                print("control did not pass; the sweep cannot decide anything")
                return 1
            digests[binary] = code_digest(TESTS / binary)
        for name, path, old, new, binary in MUTANTS:
            if only and only not in name:
                continue
            text = (backup / path).read_text()
            if text.count(old) != 1:
                print(f"[ANCHOR]   {name}: matched {text.count(old)} times, not once")
                failures += 1
                continue
            (ROOT / path).write_text(text.replace(old, new))
            try:
                verdict, _ = build_and_run(binary, scratch, digests[binary])
            finally:
                restore()
            caught = verdict in ("FAIL", "CRASH", "HANG")
            print(f"[{label(verdict, caught, 'caught', 'MISSED')}]   {name:<72} {binary} -> {verdict}", flush=True)
            failures += 0 if caught else 1
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print(f"{len(MUTANTS)} mutants, {failures} not caught" if failures else f"all {len(MUTANTS)} mutants caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
