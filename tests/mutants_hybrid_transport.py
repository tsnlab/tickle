#!/usr/bin/env python3
"""Mutant sweep for the hybrid transport (DESIGN.md section 8, "Hybrid transport for multi-datagram samples").

Each entry breaks exactly one thing the code relies on - a capability not recorded, not byte-swapped, not capped, not
forgotten; a run validation condition removed; a run's units placed one unit off - and names the test binary that
must go red for it. A mutant that still passes means either the test does not test what it says, or the condition
decides nothing and should not be in the code.

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
CAPABILITY = "test_hybrid_capability"
MTU = "test_hybrid_mtu"

# (name, file, old, new, test binary that must fail)
MUTANTS = [
    (
        "capability: not recorded",
        "src/tickle.c",
        "    node->peer_ip_reassembly_max[source] = rd16(header, announce->ip_reassembly_max);\n",
        "",
        CAPABILITY,
    ),
    (
        "capability: not byte-swapped",
        "src/tickle.c",
        "    node->peer_ip_reassembly_max[source] = rd16(header, announce->ip_reassembly_max);\n",
        "    node->peer_ip_reassembly_max[source] = announce->ip_reassembly_max;\n",
        CAPABILITY,
    ),
    (
        "capability: kept across a presumed death",
        "src/tickle.c",
        "    node->peer_ip_reassembly_max[source] = 0; // a node heard again",
        "    (void)0; // a node heard again",
        CAPABILITY,
    ),
    (
        "capability: not capped by the receive buffer (rmw build)",
        "src/tickle.c",
        "    return (uint16_t)(setting < (uint32_t)tt_MAX_BUFFER_LENGTH ? setting : (uint32_t)tt_MAX_BUFFER_LENGTH);",
        "    return (uint16_t)setting;",
        CAPABILITY,
    ),
    (
        "capability: not capped by the receive buffer (core defaults)",
        "src/tickle.c",
        "    return (uint16_t)(setting < (uint32_t)tt_MAX_BUFFER_LENGTH ? setting : (uint32_t)tt_MAX_BUFFER_LENGTH);",
        "    return (uint16_t)setting;",
        MTU,
    ),
    (
        "capability: a fragmented announce does not carry it",
        "src/tickle.c",
        "        announce->entity_count = (uint8_t)entity_count;\n        fill_announce_capability(node, announce);\n",
        "        announce->entity_count = (uint8_t)entity_count;\n",
        CAPABILITY,
    ),
    (
        "capability: a whole announce does not carry it",
        "src/tickle.c",
        "    announce->entity_count = 0;\n    fill_announce_capability(node, announce);\n",
        "    announce->entity_count = 0;\n",
        CAPABILITY,
    ),
    (
        "settings: a context's INHERIT kept rather than read as AUTO",
        "src/tickle.c",
        "                                    ? (uint8_t)tt_LARGE_FRAGMENTATION_AUTO\n",
        "                                    ? (uint8_t)tt_LARGE_FRAGMENTATION_INHERIT\n",
        CAPABILITY,
    ),
    (
        "settings: a Publisher does not inherit by default",
        "src/tickle.c",
        "    pub->large_fragmentation = tt_LARGE_FRAGMENTATION_INHERIT; // the context's",
        "    pub->large_fragmentation = tt_LARGE_FRAGMENTATION_MTU; // the context's",
        CAPABILITY,
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
        run = subprocess.run([str(TESTS / binary)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired as hung:
        return "HANG", str(hung.stdout or "")[-1000:]
    if run.returncode < 0:
        return "CRASH", run.stdout[-1000:] + run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    files = sorted({m[1] for m in MUTANTS})
    backup = pathlib.Path(tempfile.mkdtemp(prefix="hybrid_mutants_"))
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
            print(f"[{'caught' if caught else 'MISSED'}]   {name:<64} {binary} -> {verdict}")
            failures += 0 if caught else 1
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print(f"{len(MUTANTS)} mutants, {failures} not caught" if failures else f"all {len(MUTANTS)} mutants caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
