#!/usr/bin/env python3
"""Mutant sweep for the measured timer lateness, G of both retry timers (timer_lateness_fold() and its callers,
src/tickle.c; 2026-10-08, ROADMAP Now 5a).

Each entry breaks the measurement or its use in exactly one way and names the test in tests/test_timer_lateness.c
that must go red for it. A mutant the tests pass means they cannot see the thing they are named for. The unmutated
tree runs first and must pass: a test that fails on correct code would "catch" every mutant for the wrong reason.

Run from the repository root. Restores every file it touches, whatever happens, by rewriting it and moving its mtime,
so make rebuilds from the restored source rather than reusing the mutant's objects.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_timer_lateness"
TEST_FILE = "test_timer_lateness.c"

MUTANTS = [
    (
        "a wait that ran to its deadline not folded",
        "src/tickle.c",
        "            timer_lateness_fold(node, woke - until);",
        "            (void)until;",
        "test_lateness_converges_on_how_late_the_loop_wakes",
    ),
    (
        "lateness timed from when the wait began, not from its deadline",
        "src/tickle.c",
        "            timer_lateness_fold(node, woke - until);",
        "            timer_lateness_fold(node, woke - time);",
        "test_lateness_converges_on_how_late_the_loop_wakes",
    ),
    (
        "a wait cut short counted as a sample",
        "src/tickle.c",
        "        if (until != UINT64_MAX && woke >= until) {",
        "        if (until != UINT64_MAX) {",
        "test_a_wait_cut_short_is_not_a_sample",
    ),
    (
        "a wait with no deadline counted as a sample",
        "src/tickle.c",
        "        if (until != UINT64_MAX && woke >= until) {",
        "        if (woke >= until || until == UINT64_MAX) {",
        "test_a_wait_cut_short_is_not_a_sample",
    ),
    (
        "a budget's deadline not counted",
        "src/tickle.c",
        "        if (until != UINT64_MAX && woke >= until) {",
        "        if (until_next_event && until != UINT64_MAX && woke >= until) {",
        "test_a_budget_wait_is_a_sample",
    ),
    (
        "no floor",
        "src/tickle.c",
        "    if (g < floor) {\n        g = floor;",
        "    if (g < floor && floor == 0) {\n        g = floor;",
        "test_lateness_is_floored_at_the_timer_resolution",
    ),
    (
        "the floor not taken from the HAL at create",
        "src/tickle.c",
        "    node->timer_resolution_ns = resolution > UINT32_MAX ? UINT32_MAX : (uint32_t)resolution;",
        "    node->timer_resolution_ns = resolution > UINT32_MAX ? UINT32_MAX : 1U;",
        "test_lateness_is_floored_at_the_timer_resolution",
    ),
    (
        "G the mean alone, without the deviation",
        "src/tickle.c",
        "    uint64_t g = (uint64_t)node->timer_lateness_mean_ns + (4ULL * node->timer_lateness_var_ns);",
        "    uint64_t g = (uint64_t)node->timer_lateness_mean_ns;",
        "test_a_jittering_timer_gives_more_than_its_mean",
    ),
    (
        "no cold start: G 0 until the first sample",
        "src/tickle.c",
        "    return g != 0 ? (uint64_t)g : (uint64_t)tt_TIMER_LATENESS_INITIAL;",
        "    return (uint64_t)g;",
        "test_cold_start_is_the_old_constant",
    ),
    (
        "the reliable retry keeps the fixed 100 us",
        "src/tickle.c",
        "    if (spread < granularity) {\n        spread = granularity;\n    }\n    uint64_t interval = srtt + spread;",
        "    if (spread < (uint64_t)tt_TIMER_LATENESS_INITIAL) {\n        spread = (uint64_t)tt_TIMER_LATENESS_INITIAL;\n"
        "    }\n    uint64_t interval = srtt + spread;",
        "test_the_retry_intervals_use_the_measured_lateness",
    ),
    (
        "the reliable retry handed the cold G",
        "src/tickle.c",
        "    return retry_interval_for(reliable_retry_configured(), reliable_retry_granularity(node), proxy);",
        "    return retry_interval_for(reliable_retry_configured(), reliable_retry_granularity(NULL), proxy);",
        "test_the_retry_intervals_use_the_measured_lateness",
    ),
    (
        "the repair-in-flight window keeps the cold G",
        "src/tickle.c",
        "    uint64_t granularity = reliable_retry_granularity(node);\n    return (uint64_t)proxy->transit_srtt_ns",
        "    uint64_t granularity = reliable_retry_granularity(NULL);\n    return (uint64_t)proxy->transit_srtt_ns",
        "test_the_retry_intervals_use_the_measured_lateness",
    ),
    (
        "the call's G not doubled",
        "src/tickle.c",
        "    return configured != 0 ? configured : 2U * reliable_retry_granularity(node);",
        "    return configured != 0 ? configured : reliable_retry_granularity(node);",
        "test_cold_start_is_the_old_constant",
    ),
    (
        "the call keeps the cold G",
        "src/tickle.c",
        "    uint64_t granularity = call_retry_granularity(client->node);",
        "    uint64_t granularity = call_retry_granularity(NULL);",
        "test_the_retry_intervals_use_the_measured_lateness",
    ),
    (
        "a fixed G ignored",
        "src/tickle.c",
        "    return configured != 0 ? configured : timer_lateness_ns(node);",
        "    return timer_lateness_ns(node);",
        "test_a_fixed_granularity_overrides_the_measurement",
    ),
]


def build_and_run():
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True, cwd=ROOT / "platform/linux", check=False,
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    run = subprocess.run([str(BIN)], capture_output=True, text=True, check=False, timeout=600)
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def guard_lines(test: str) -> range:
    """The line range of `test`'s body in the test file, so a failure can be attributed to it."""
    lines = (ROOT / "tests" / TEST_FILE).read_text().splitlines()
    start = next(i for i, line in enumerate(lines, 1) if line.startswith(f"static void {test}(void)"))
    end = next(i for i, line in enumerate(lines[start:], start + 1) if line == "}")
    return range(start, end + 1)


def failed_in(output: str, test: str) -> bool:
    span = guard_lines(test)
    for line in output.splitlines():
        if f"{TEST_FILE}:" in line:
            try:
                number = int(line.split(f"{TEST_FILE}:")[1].split(":")[0])
            except ValueError:
                continue
            if number in span:
                return True
    return False


def main():
    backup = tempfile.mkdtemp(prefix="lateness_mutants_")
    touched = {m[1] for m in MUTANTS}
    for rel in touched:
        shutil.copy2(ROOT / rel, pathlib.Path(backup) / rel.replace("/", "_"))

    def restore():
        for rel in touched:
            # copy(), then touch: a restored file must look newer than the objects built from the mutant.
            shutil.copy(pathlib.Path(backup) / rel.replace("/", "_"), ROOT / rel)
            (ROOT / rel).touch()

    failures = 0
    try:
        verdict, output = build_and_run()
        print(f"[control]  unmutated -> {verdict}")
        if verdict != "PASS":
            print(output[-2000:])
            print("control did not pass; the sweep cannot decide anything")
            return 1
        for name, rel, old, new, test in MUTANTS:
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR]    {name} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            verdict, output = build_and_run()
            restore()
            # Caught means the tests failed, and among the failures is the guard named for this mutant.
            ok = verdict == "FAIL" and failed_in(output, test)
            failures += 0 if ok else 1
            print(f"[{'ok' if ok else 'SURVIVED':8s}] {name} -> {verdict} (guard {test}"
                  f"{'' if failed_in(output, test) else ': did not fail'})")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
