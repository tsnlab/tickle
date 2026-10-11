#!/usr/bin/env python3
"""Mutant sweep for the hybrid transport's sending half (DESIGN.md section 8, "Hybrid transport", steps 3 to 5).

Each entry breaks exactly one condition the sender relies on - when a sample may go in runs, how n is bounded, where a
run ends, what a repair is, how loss and cost are counted - and names the test binary that must go red for it. A mutant
that still passes means either the test does not test what it says, or the condition decides nothing and should not be
in the code.

Run from the repository root. Restores every file it touches, whatever happens. MUTANTS=<substring> runs only the
mutants whose name contains it (the controls always run).
"""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TESTS = ROOT / "platform/linux/obj/debug/tests"
SEND = "test_hybrid_send"
SHM = "test_large_shm"

# (name, file, old, new, test binary that must fail)
MUTANTS = [
    (
        "runs: from two units, not three",
        "src/tickle.c",
        "        if (pub != NULL && count >= 3U) {",
        "        if (pub != NULL && count >= 2U) {",
        SEND,
    ),
    (
        "small runs: the last unit inside a run",
        "src/tickle.c",
        "    for (uint32_t start = 0; start + 1U < count; start += n) {\n"
        "        uint32_t units = count - 1U - start < n ? count - 1U - start : n;",
        "    for (uint32_t start = 0; start < count; start += n) {\n"
        "        uint32_t units = count - start < n ? count - start : n;",
        SEND,
    ),
    (
        "small runs: each run named by the sample's seq_no, not its first unit's",
        "src/tickle.c",
        "    run->data.seq_no = data_header->seq_no + start; // the first unit's own",
        "    run->data.seq_no = data_header->seq_no; // the first unit's own",
        SEND,
    ),
    (
        "small runs: the tail not sent",
        "src/tickle.c",
        "    return hybrid_send_batch(node, &last, 1, &dest->context_id, 1) >= 0;",
        "    return true;",
        SEND,
    ),
    (
        "large runs: the last unit inside a run",
        "src/tickle.c",
        "    for (; k < LARGE_BATCH && next < last; k++) {\n        uint32_t run = last - next < n ? last - next : n;",
        "    for (; k < LARGE_BATCH && next <= last; k++) {\n"
        "        uint32_t run = last + 1U - next < n ? last + 1U - next : n;",
        SEND,
    ),
    (
        "large runs: each run named by the sample's seq_no",
        "src/tickle.c",
        "        header_l->data.seq_no = data->seq_no + next; // the first unit's own",
        "        header_l->data.seq_no = data->seq_no; // the first unit's own",
        SEND,
    ),
    (
        "large runs: the cursor moves by datagrams, not units",
        "src/tickle.c",
        "            moved += units[k];",
        "            moved += 1U;",
        SEND,
    ),
    (
        "n_max: the peer's ceiling ignored",
        "src/tickle.c",
        "    uint32_t units = ceiling > framing ? (ceiling - framing) / unit : 0U;",
        "    uint32_t units = (uint32_t)tt_HYBRID_N_MAX;",
        SEND,
    ),
    (
        "n_max: the Publisher's ip_datagram_max ignored",
        "src/tickle.c",
        "    if (pub->ip_datagram_max != 0 && pub->ip_datagram_max < ceiling) {",
        "    if (false) {",
        SEND,
    ),
    (
        "n_max: a segment peer gets runs",
        "src/tickle.c",
        "    if (n_max > 1U && peer_segment(node, context_id, ip, port) != NULL) {",
        "    if (false) {",
        SHM,
    ),
    (
        "mode: MTU ignored",
        "src/tickle.c",
        "    if (n_max < 2U || mode == tt_LARGE_FRAGMENTATION_MTU) {",
        "    if (n_max < 2U) {",
        SEND,
    ),
    (
        "mode: a Publisher's override ignored",
        "src/tickle.c",
        "    return pub->large_fragmentation != tt_LARGE_FRAGMENTATION_INHERIT ? pub->large_fragmentation\n"
        "                                                                       : node->large_fragmentation;",
        "    UNUSED(pub);\n    return node->large_fragmentation;",
        SEND,
    ),
    (
        "mode: RELIABLE AUTO sent n_max as best effort is",
        "src/tickle.c",
        "    if (mode == tt_LARGE_FRAGMENTATION_IP || !pub->reliable) {",
        "    if (true) {",
        SEND,
    ),
    (
        "broadcast: the largest n over the peers, not the smallest",
        "src/tickle.c",
        "        smallest = smallest == 0 || n < smallest ? n : smallest;",
        "        smallest = smallest == 0 || n > smallest ? n : smallest;",
        SEND,
    ),
    (
        "unicast: every destination given the broadcast's minimum",
        "src/tickle.c",
        "    uint32_t n_max = 1U;\n    if (context_id != tt_CONTEXT_ID_INVALID) {",
        "    uint32_t n_max = 1U;\n    if (false) {",
        SEND,
    ),
    (
        "counters: a run's units not counted",
        "src/tickle.c",
        "        hybrid->run_units += count - 1U;",
        "",
        SEND,
    ),
]


def build_and_run(binary):
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), f"obj/debug/tests/{binary}"],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    try:
        run = subprocess.run([str(TESTS / binary)], capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired as hung:
        return "HANG", str(hung.stdout or "")[-1000:]
    if run.returncode < 0:
        return "CRASH", run.stdout[-1000:] + run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    files = sorted({m[1] for m in MUTANTS})
    backup = pathlib.Path(tempfile.mkdtemp(prefix="hybrid_send_mutants_"))
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
    try:
        for binary in sorted({m[4] for m in MUTANTS}):
            verdict, output = build_and_run(binary)
            print(f"[control]  {binary:<28} unmutated -> {verdict}")
            if verdict != "PASS":
                print(output[-2000:])
                print("control did not pass; the sweep cannot decide anything")
                return 1
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
                verdict, _ = build_and_run(binary)
            finally:
                restore()
            caught = verdict in ("FAIL", "CRASH", "HANG")
            print(f"[{'caught' if caught else 'MISSED'}]   {name:<72} {binary} -> {verdict}")
            failures += 0 if caught else 1
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print(f"{len(MUTANTS)} mutants, {failures} not caught" if failures else f"all {len(MUTANTS)} mutants caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
