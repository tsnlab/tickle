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

# (name, file, text, replacement[, round trips[, runs]]). The fence mutant's race is the rarest, and was rarer still
# from cd09e895 on (0 lost wake-ups in 3.2 million round trips, against 1-6 in 400,000 before): bell_wake_check aims half
# its pongs at the reader's announcement and clogs the writer's store buffer while it publishes them, and then lost the
# first wake-up within 283-20,789 round trips in 40 runs of 40 on the PC. It still runs 400,000 for the margin; a run
# stops at its first lost wake-up, so the margin costs a caught mutant nothing.
# Since a called-off sleep keeps its generation (segment_sleep_called_off(), 2026-10-08) the reader no longer takes the
# extra sleeps that a chain of stale rings gave it - 0.75 sleeps a round trip, every one ended by the bell, against
# 0.77-0.81 with a tenth not - and the race has fewer announcements to land on: on the PC 9 runs of 30 kept the fence
# mutant alive through 400,000 round trips, while the others lost a wake-up within 67-283,181. Runs are independent,
# so the mutant gets up to six (all six surviving: ~0.1% at that rate) and is caught by the first that loses one.
MUTANTS = [
    (
        "the writer's fence between publishing a record and reading reader_waiting (x86 store-buffer reordering)",
        "src/tickle.c",
        "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n    uint32_t sleeping = __atomic_load_n(&segment->reader_waiting",
        "    uint32_t sleeping = __atomic_load_n(&segment->reader_waiting",
        400000,
        6,
    ),
    (
        "the reader's second drain, after it says it is about to sleep (the classic race)",
        "src/tickle.c",
        "    if (drain_own_segment(node, &emptied_before_wait) > 0 || !emptied_before_wait) {\n"
        "        segment_sleep_called_off(node);",
        "    if (false) {\n        segment_sleep_called_off(node);",
    ),
    (
        "the writer ringing each new sleep generation, not only the first it saw",
        "src/tickle.c",
        "        if (sleeping != peer->doorbell_generation) {",
        "        if (peer->doorbell_generation == 0) {",
    ),
    (
        "the edge-triggered bell ever being read (its pipe fills and refuses every ring)",
        "src/hal_linux.c",
        "    if (generation - node->hal.bell_drained_at >= node->hal.bell_drain_every) {",
        "    if (false) {",
    ),
    (
        "the bell read before it can fill (read at twice the pipe's capacity in generations)",
        "src/hal_linux.c",
        "    uint32_t every = capacity > 0 ? (uint32_t)capacity / (4U * (uint32_t)tt_MAX_CONTEXT_IDS) : 0U;",
        "    uint32_t every = capacity > 0 ? (uint32_t)capacity * 2U : 0U;",
    ),
    (
        "the bell joining the wait set at all",
        "src/hal_linux.c",
        "    if (epoll_ctl(node->hal.epoll_fd_plus1 - 1, EPOLL_CTL_ADD, bell, &event) != 0) {",
        "    if (false) {",
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
        runs = entry[5] if len(entry) > 5 else 1
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
            for attempt in range(1, runs + 1):
                code, said = run(round_trips)
                if code != 0:
                    break
            said = f"run {attempt} of {runs}: {said}" if runs > 1 else said
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
