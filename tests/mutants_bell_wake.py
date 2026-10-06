#!/usr/bin/env python3
"""Mutant sweep for platform/linux/bell_wake_check.c, the same-host lost-wake-up check.

Each entry breaks the reader/writer wake-up protocol in exactly one way that loses a wake-up, and
bell_wake_check must fail on every one. A mutant it passes means the check cannot see the race it is
named for. The unmutated tree runs first and must pass: a check that fails on correct code would
"catch" every mutant for the wrong reason.

Runs each build in a private network namespace (examples/perf_hil/experiments/bell_wake_netns.sh).
Run from the repository root. Restores every file it touches, whatever happens, by rewriting it -
which moves its mtime, so make rebuilds from the restored source rather than reusing the mutant's
objects.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "examples/perf_hil/experiments/bell_wake_netns.sh"
BIN = ROOT / "platform/linux/bell_wake_check"

# (name, file, text, replacement[, round trips]). The fence mutant's race is rarer - about 3.5 lost wake-ups per
# 100,000 round trips on the PC it was found on - so it runs 400,000, where a run that sees none is e^-14 likely.
MUTANTS = [
    (
        "the writer's fence between publishing a record and reading reader_waiting (x86 store-buffer reordering)",
        "src/tickle.c",
        "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n    uint32_t sleeping = __atomic_load_n(&segment->reader_waiting",
        "    uint32_t sleeping = __atomic_load_n(&segment->reader_waiting",
        400000,
    ),
    (
        "the reader's second drain, after it says it is about to sleep (the classic race)",
        "src/tickle.c",
        "    if (drain_own_segment(node, &emptied_before_wait) > 0 || !emptied_before_wait) {\n"
        "        segment_reader_waiting(node, false);",
        "    if (false) {\n        segment_reader_waiting(node, false);",
    ),
    (
        "the writer ringing each new sleep generation, not only the first it saw",
        "src/tickle.c",
        "        if (sleeping != peer->doorbell_generation) {",
        "        if (peer->doorbell_generation == 0) {",
    ),
]


def build() -> bool:
    result = subprocess.run(["make", "-C", "platform/linux", "bell_wake_check"], cwd=ROOT, capture_output=True,
                            text=True, check=False)
    if result.returncode != 0:
        print(result.stdout[-2000:], result.stderr[-2000:])
    return result.returncode == 0


def run(round_trips: int = 0) -> tuple[int, str]:
    args = [str(RUNNER), str(BIN)] + ([str(round_trips)] if round_trips else [])
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=False,
                            timeout=300)
    lines = [line for line in result.stdout.splitlines()
             if line.startswith("bell_wake_check:") and "lost wake-up:" not in line]
    return result.returncode, " | ".join(lines)


def main() -> int:
    if not build():
        print("FATAL: the unmutated tree does not build")
        return 1
    code, said = run()
    print(f"control (unmutated): exit {code}: {said}")
    if code != 0:
        print("FATAL: bell_wake_check fails on the unmutated tree; a mutant failing proves nothing")
        return 1
    survivors = 0
    for entry in MUTANTS:
        name, path, old, new = entry[:4]
        round_trips = entry[4] if len(entry) > 4 else 0
        target = ROOT / path
        original = target.read_text()
        if original.count(old) != 1:
            print(f"FATAL: mutant '{name}': the text to replace occurs {original.count(old)} times in {path}")
            return 1
        try:
            target.write_text(original.replace(old, new))
            if not build():
                print(f"FATAL: mutant '{name}' does not build")
                return 1
            code, said = run(round_trips)
        finally:
            target.write_text(original)
        caught = code == 1
        survivors += 0 if caught else 1
        print(f"{'CAUGHT ' if caught else 'SURVIVED'} {name}: exit {code}: {said}")
    if not build():
        print("FATAL: the restored tree does not build")
        return 1
    print(f"{len(MUTANTS) - survivors} of {len(MUTANTS)} mutants caught")
    return 0 if survivors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
