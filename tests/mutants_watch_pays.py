#!/usr/bin/env python3
"""Mutant sweep for the reader's choice of whether its waits pay (segment_epoch_turn(), src/tickle.c), and for
what it takes a sleep to cost (segment_resumed()).

Each entry breaks the choice in exactly one way and names the test in tests/test_transport_seam.c that must go red
for it. A mutant the tests pass means they cannot see the thing they are named for. The unmutated tree runs first and
must pass: a test that fails on correct code would "catch" every mutant for the wrong reason.

Run from the repository root. Restores every file it touches, whatever happens, by rewriting it and moving its mtime,
so make rebuilds from the restored source rather than reusing the mutant's objects.
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
        "the writer's half of the cost (wall time per record) left out",
        "src/tickle.c",
        "        const uint64_t spent = (now - node->segment_epoch_ns) + (cpu - node->segment_epoch_cpu_ns);",
        "        const uint64_t spent = (cpu - node->segment_epoch_cpu_ns);",
        "test_the_choice_weighs_both_the_writer_and_the_reader",
    ),
    (
        "the reader's half of the cost (its CPU time per record) left out",
        "src/tickle.c",
        "        const uint64_t spent = (now - node->segment_epoch_ns) + (cpu - node->segment_epoch_cpu_ns);",
        "        const uint64_t spent = (now - node->segment_epoch_ns);",
        "test_the_choice_weighs_both_the_writer_and_the_reader",
    ),
    (
        "the dearer mode chosen",
        "src/tickle.c",
        "    if (verdict < 0) {\n        node->segment_preferred = (uint8_t)(preferred ^ 1U);",
        "    if (verdict > 0) {\n        node->segment_preferred = (uint8_t)(preferred ^ 1U);",
        "test_the_waits_are_taken_where_they_cost_the_pair_less",
    ),
    (
        "the empty-ring watch taken whatever the measured mode",
        "src/tickle.c",
        "    if (node->segment_watching == 0 || budget == 0 || node->segment_gap_ns >= budget) {",
        "    if (budget == 0 || node->segment_gap_ns >= budget) {",
        "test_the_waits_are_declined_where_they_cost_the_pair_more",
    ),
    (
        "the claim wait taken whatever the measured mode",
        "src/tickle.c",
        "        (node->segment_watching == 0 || deadline <= time || !segment_await_claim(node, time, deadline))) {",
        "        (deadline <= time || !segment_await_claim(node, time, deadline))) {",
        "test_the_waits_are_declined_where_they_cost_the_pair_more",
    ),
    (
        "single epochs compared, not means against their standard error",
        "src/tickle.c",
        "    if (magnitude * magnitude <= 4U * se2) {",
        "    if (magnitude == 0 && se2 == se2) {",
        "test_a_noisy_epoch_does_not_turn_the_choice",
    ),
    (
        "the re-measure interval never doubling",
        "src/tickle.c",
        "            node->segment_probe_every *= 2; // a re-measure that did not change the choice: the next one later",
        "            node->segment_probe_every *= 1; // mutant",
        "test_the_waits_are_declined_where_they_cost_the_pair_more",
    ),
    (
        "the mode not chosen never measured again",
        "src/tickle.c",
        "    return other; // time to measure the other mode again",
        "    return preferred; // mutant",
        "test_the_choice_follows_a_change_in_the_costs",
    ),
    (
        "the window of recent epochs (the modes' records never halved)",
        "src/tickle.c",
        "        if ((node->segment_epochs[0] + node->segment_epochs[1]) % tt_SEGMENT_PROBE_EVERY_MAX == 0) {",
        "        if (node->segment_epochs[0] == 0xFFFFFFFFU) {",
        "test_the_choice_follows_a_change_in_the_costs",
    ),
    (
        "the shortest clean sleep kept as what a sleep costs, not the mean",
        "src/tickle.c",
        "    node->segment_sleep_cost_ns = (uint64_t)(mean + (((int64_t)cost - mean) / (int64_t)count));",
        "    node->segment_sleep_cost_ns = (count == 1 || cost < (uint64_t)mean) ? cost : (uint64_t)mean;",
        "test_a_sleep_that_never_blocked_does_not_become_the_bound",
    ),
    (
        "the first epoch after a change of mode measured",
        "src/tickle.c",
        "    const bool settled = node->segment_epoch_settling == 0;",
        "    const bool settled = true;",
        "test_the_first_epoch_after_a_change_is_not_measured",
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
    """The line range of `test`'s body in test_transport_seam.c, so a failure can be attributed to it."""
    lines = (ROOT / "tests/test_transport_seam.c").read_text().splitlines()
    start = next(i for i, line in enumerate(lines, 1) if line.startswith(f"static void {test}(void)"))
    end = next(i for i, line in enumerate(lines[start:], start + 1) if line == "}")
    return range(start, end + 1)


def failed_in(output: str, test: str) -> bool:
    span = guard_lines(test)
    for line in output.splitlines():
        if "test_transport_seam.c:" in line:
            try:
                number = int(line.split("test_transport_seam.c:")[1].split(":")[0])
            except ValueError:
                continue
            if number in span:
                return True
    return False


def main():
    backup = tempfile.mkdtemp(prefix="watch_mutants_")
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
