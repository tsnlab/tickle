#!/usr/bin/env python3
"""Mutant sweep for the two clocks (2026-10-16, DESIGN.md section 2 "Time"): every deadline on tt_get_ns(), the
monotonic clock, and only what leaves the process as a point in time on tt_get_wall_ns(), the date.

Each entry puts one site on the wrong clock and names the check that must go red for it:
  core        tests/test_clock_step.c, the default build
  core_local  tests/test_clock_step.c with -Dtt_LOCAL_DELIVERY=1 (rmw_tickle's build), for the local-delivery stamp
  hal         platform/linux/clock_check.c against the real Linux HAL
A mutant the check passes means it cannot see the clock it is named for.

Every publish path (batched, zero-copy, same-host ring record, claimed slot, large sample) takes its date from
publish_clock() since 2026-10-16, one monotonic read plus wall_offset_ns, so the publish mutants below are on that one
function; test_clock_step.c also counts the clock reads a publish takes, which is what sees the mutant that reads the
date a second time - correct stamps, the old cost. The unmutated tree runs first and every check must pass: a check
that fails on correct code would "catch" every mutant for the wrong reason.

Run from the repository root (TMPDIR outside /tmp if it is small). Each file is restored by rewriting it and moving
its mtime, so make rebuilds from the restored source rather than reusing the mutant's objects; the core checks are
compiled directly, without make, into a temporary directory.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
LINUX = ROOT / "platform/linux"
RUN_TIMEOUT_S = 120  # an unmutated run takes ~2 s at -O0

MUTANTS = [
    (
        "a publish stamps the deadline clock",
        "src/tickle.c",
        "    *wall_ns = now + node->wall_offset_ns;",
        "    *wall_ns = now;",
        "core",
    ),
    (
        "a publish reads the date a second time",
        "src/tickle.c",
        "    *wall_ns = now + node->wall_offset_ns;",
        "    *wall_ns = tt_get_wall_ns();",
        "core",
    ),
    (
        "a publish reads the clock again for what it retains",
        "src/tickle.c",
        "    return node->publish_clock_ns != 0 ? node->publish_clock_ns : tt_get_ns();",
        "    return tt_get_ns();",
        "core",
    ),
    # A scheduled publish takes the poll's reading (2026-10-22): the first publish of an entry the poll ran at a fresh
    # reading, once. test_clock_step.c's scheduled case counts the reads and checks each stamp.
    (
        "a scheduled publish reads the clock again (the core before 2026-10-22)",
        "src/tickle.c",
        "    uint64_t now = node->entry_clock_ns;\n    if (now != 0) {",
        "    uint64_t now = 0;\n    if (now != 0) {",
        "core",
    ),
    (
        "the poll's reading not consumed: a second publish in the entry stamps it too",
        "src/tickle.c",
        "    if (now != 0) {\n        node->entry_clock_ns = 0;\n    } else {",
        "    if (now != 0) {\n    } else {",
        "core",
    ),
    (
        "the reading left set after the entry: a publish outside the poll takes it",
        "src/tickle.c",
        "        node->entry_clock_ns = 0; // the entry is over: its reading is not the next one's\n",
        "",
        "core",
    ),
    (
        "every entry of a non-blocking poll at the first's reading",
        "src/tickle.c",
        "        fresh = false;\n    }",
        "    }",
        "core",
    ),
    (
        "an entry handed a reading it was not run at",
        "src/tickle.c",
        "        node->entry_clock_ns = fresh ? now : 0;",
        "        node->entry_clock_ns = now;",
        "core",
    ),
    (
        "the blocking poll's entries not at a fresh reading",
        "src/tickle.c",
        "            ran = run_due_entry_reading(node, time, true, &has_next, &next);",
        "            ran = run_due_entry_reading(node, time, false, &has_next, &next);",
        "core",
    ),
    (
        "the receiver rebuilds against the deadline clock",
        "src/tickle.c",
        "    int64_t now_us = (int64_t)(rx_wall_now(node) / tt_MICROSECOND);",
        "    int64_t now_us = (int64_t)(rx_now(node) / tt_MICROSECOND);",
        "core",
    ),
    (
        "the poll's reading taken as the date without the offset",
        "src/tickle.c",
        "    return node->rx_clock_ns != 0 ? node->rx_clock_ns + node->wall_offset_ns : tt_get_wall_ns();",
        "    return node->rx_clock_ns != 0 ? node->rx_clock_ns : tt_get_wall_ns();",
        "core",
    ),
    (
        "the offset never read again after creation",
        "src/tickle.c",
        "    wall_offset_refresh(node); // once a tt_CONTEXT_UPDATE_INTERVAL: a step of the date is followed within it\n",
        "",
        "core",
    ),
    (
        "the writer's lifespan on the date",
        "src/tickle.c",
        "    return lifespan_duration_ns != 0 && (tt_get_ns() - entry->timestamp) >= lifespan_duration_ns;",
        "    return lifespan_duration_ns != 0 && (tt_get_wall_ns() - entry->timestamp) >= lifespan_duration_ns;",
        "core",
    ),
    (
        "the poll loop (scheduler, liveliness, retries) on the date",
        "src/tickle.c",
        "    uint64_t time = tt_get_ns();\n    const uint64_t poll_start = time;",
        "    uint64_t time = tt_get_wall_ns();\n    const uint64_t poll_start = time;",
        "core",
    ),
    (
        "local delivery stamps the deadline clock",
        "src/tickle.c",
        "    uint64_t local_timestamp = wall_ns; // the date on the wire, to the nanosecond",
        "    uint64_t local_timestamp = now_ns;",
        "core_local",
    ),
    (
        "the HAL's deadline clock is the date",
        "src/hal_linux.c",
        "    // NOLINTNEXTLINE(misc-include-cleaner)\n    clock_gettime(CLOCK_MONOTONIC, &ts);",
        "    // NOLINTNEXTLINE(misc-include-cleaner)\n    clock_gettime(CLOCK_REALTIME, &ts);",
        "hal",
    ),
    (
        "the HAL's date is the deadline clock",
        "src/hal_linux.c",
        "    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_REALTIME, as CLOCK_MONOTONIC above\n"
        "    clock_gettime(CLOCK_REALTIME, &ts);",
        "    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_REALTIME, as CLOCK_MONOTONIC above\n"
        "    clock_gettime(CLOCK_MONOTONIC, &ts);",
        "hal",
    ),
]


def run_core(work, defines):
    binary = pathlib.Path(work) / "test_clock_step"
    build = subprocess.run(
        ["cc", f"-I{ROOT}/include", f"-I{ROOT}/src", "-O0", "-g", *defines, "-o", str(binary),
         str(ROOT / "tests/test_clock_step.c"), str(ROOT / "src/encoding.c"), str(ROOT / "src/log.c"), "-lm"],
        capture_output=True, text=True, check=False,
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    try:
        run = subprocess.run([str(binary)], capture_output=True, text=True, check=False, timeout=RUN_TIMEOUT_S)
    except subprocess.TimeoutExpired as expired:
        # The poll loop on the date never returns from the step back (seen 2026-10-16): the stream cannot pass, so the
        # mutant is caught, but by the timeout and not by a check - said apart from FAIL.
        out = (expired.stdout or b"").decode(errors="replace") + (expired.stderr or b"").decode(errors="replace")
        return "TIMEOUT", out
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def run_hal(_work, _defines):
    build = subprocess.run(["make", "-s", "-C", str(LINUX), "clock_check"], capture_output=True, text=True,
                           check=False)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    run = subprocess.run([str(LINUX / "clock_check")], capture_output=True, text=True, check=False, timeout=60)
    verdict = {0: "PASS", 1: "FAIL"}.get(run.returncode, "VOID")
    return verdict, run.stdout + run.stderr


CHECKS = {
    "core": (run_core, []),
    "core_local": (run_core, ["-Dtt_LOCAL_DELIVERY=1"]),
    "hal": (run_hal, []),
}


def summary(output):
    return " | ".join(line for line in output.splitlines()
                      if line.startswith(("forward:", "back:", "clock_check:")) or "test_clock_step.c:" in line)[:600]


def main():
    work = tempfile.mkdtemp(prefix="clock_split_mutants_", dir=os.environ.get("TMPDIR"))
    touched = sorted({m[1] for m in MUTANTS})
    for rel in touched:
        shutil.copy2(ROOT / rel, pathlib.Path(work) / rel.replace("/", "_"))

    def restore():
        for rel in touched:
            shutil.copy(pathlib.Path(work) / rel.replace("/", "_"), ROOT / rel)
            (ROOT / rel).touch()

    survivors = 0
    try:
        for check, (runner, defines) in CHECKS.items():
            verdict, output = runner(work, defines)
            print(f"[control]  {check}: unmutated -> {verdict}")
            if verdict != "PASS":
                print(output[-3000:])
                print("control did not pass; the sweep cannot decide anything")
                return 1
        for name, rel, old, new, check in MUTANTS:
            path = ROOT / rel
            text = path.read_text()
            if text.count(old) != 1:
                print(f"[STALE]    {name}: the text to mutate occurs {text.count(old)} times in {rel}")
                survivors += 1
                continue
            path.write_text(text.replace(old, new))
            try:
                runner, defines = CHECKS[check]
                verdict, output = runner(work, defines)
            finally:
                restore()
            killed = verdict in ("FAIL", "TIMEOUT")  # a run that never ends cannot pass either
            survivors += 0 if killed else 1
            print(f"[{'killed' if killed else 'SURVIVED'}] {name} ({check}) -> {verdict}: {summary(output)}")
    finally:
        restore()
        run_hal(work, [])  # rebuild clock_check from the restored source
        shutil.rmtree(work, ignore_errors=True)
    print(f"{len(MUTANTS) - survivors} of {len(MUTANTS)} killed")
    return 0 if survivors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
