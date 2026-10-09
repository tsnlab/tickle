#!/usr/bin/env python3
"""Mutant sweep for the segment ownership check (2026-10-09): a writer never uses a segment whose owner is dead.

Each entry removes exactly one mechanism; tests/test_segment_owner.c must go red for every one. A mutant that
still passes means the test is not testing what it says. Run from the repository root; it restores every file it
touches, whatever happens, and touches them after so make rebuilds from the restored source.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_segment_owner"

MUTANTS = [
    (
        "the writer asking whether the owner lives",
        "src/hal_linux.c",
        "    if (segment_owner_gone(segment_fd)) {\n        *why = (uint8_t)tt_SEGMENT_ORPHANED;",
        "    if (false) {\n        *why = (uint8_t)tt_SEGMENT_ORPHANED;",
    ),
    (
        "the owner holding its lock",
        "src/hal_linux.c",
        "    if (flock(segment_fd, LOCK_EX) != 0) {",
        "    if (false) {",
    ),
    (
        "the writer's check being shared, not exclusive",
        "src/hal_linux.c",
        "    return flock(segment_fd, LOCK_SH | LOCK_NB) == 0;",
        "    return flock(segment_fd, LOCK_EX | LOCK_NB) == 0;",
    ),
    (
        "the check reading 'held' as alive",
        "src/hal_linux.c",
        "    return flock(segment_fd, LOCK_SH | LOCK_NB) == 0;",
        "    return true;",
    ),
]


def build_and_run():
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=120)
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    backup = tempfile.mkdtemp(prefix="segment_owner_mutants_")
    touched = {m[1] for m in MUTANTS}
    for rel in touched:
        shutil.copy(ROOT / rel, pathlib.Path(backup) / rel.replace("/", "_"))

    def restore():
        for rel in touched:
            shutil.copy(pathlib.Path(backup) / rel.replace("/", "_"), ROOT / rel)
            (ROOT / rel).touch()

    failures = 0
    try:
        verdict, output = build_and_run()
        print(f"[control]  unmutated                                     -> {verdict}")
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1
        for name, rel, old, new in MUTANTS:
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR]    {name:46s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            verdict, output = build_and_run()
            restore()
            ok = verdict == "FAIL"
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name:46s} -> {verdict} (wanted FAIL)")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
