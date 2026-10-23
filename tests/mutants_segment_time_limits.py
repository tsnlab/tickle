#!/usr/bin/env python3
"""Mutant sweep for ROADMAP 5a's counts made times (2026-10-22): the segment answers aged by the summary tick.

Each entry removes exactly one condition; tests/test_segment_time_limits.c must go red for every one. A mutant that
still passes means the test is not testing what it says. Run from the repository root; it restores every file it
touches, whatever happens, and touches them after so make rebuilds from the restored source.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_segment_time_limits"

MUTANTS = [
    # A "no" (a peer with no segment for us).
    (
        "a send asking whenever a no is cached",
        "src/tickle.c",
        "        } else if (!entry->recheck_due) {\n            return NULL; // asked recently",
        "        } else if (false) {\n            return NULL; // asked recently",
    ),
    (
        "a send never asking about a no again",
        "src/tickle.c",
        "        } else if (!entry->recheck_due) {\n            return NULL; // asked recently",
        "        } else if (true) {\n            return NULL; // asked recently",
    ),
    (
        "a no due at the tick that stamps it",
        "src/tickle.c",
        "            entry->recheck_at = tick + segment_recheck_span(stands);",
        "            entry->recheck_at = tick;",
    ),
    (
        "the stamp never kept",
        "src/tickle.c",
        "            entry->recheck_stamped = true;\n",
        "",
    ),
    (
        "a deadline met only past it",
        "src/tickle.c",
        "        if ((int32_t)(tick - entry->recheck_at) >= 0) {",
        "        if ((int32_t)(tick - entry->recheck_at) > 0) {",
    ),
    (
        "the sweep never making an answer due",
        "src/tickle.c",
        "        if ((int32_t)(tick - entry->recheck_at) >= 0) {\n            entry->recheck_due = true;",
        "        if ((int32_t)(tick - entry->recheck_at) >= 0) {\n            (void)entry;",
    ),
    (
        "the summary tick not aging the answers",
        "src/tickle.c",
        "    segment_recheck_sweep(node, time); // the cached segment answers age here, not per send\n",
        "",
    ),
    (
        "a no refused again left due",
        "src/tickle.c",
        "    entry->missing = true;\n    entry->recheck_due = false;\n",
        "    entry->missing = true;\n",
    ),
    (
        "a no refused again keeping its old stamp",
        "src/tickle.c",
        "    entry->recheck_due = false;\n    entry->recheck_stamped = false;\n}",
        "    entry->recheck_due = false;\n}",
    ),
    (
        "a same-host announce not clearing a no",
        "src/tickle.c",
        "        if (entry->mapping == NULL && entry->missing) {\n            entry->recheck_due = true;",
        "        if (entry->mapping == NULL && entry->missing) {\n            (void)entry;",
    ),
    # A "yes" (an attached segment).
    (
        "an attached send never revalidating",
        "src/tickle.c",
        "    if (!entry->recheck_due) {\n        return true;",
        "    if (true) {\n        return true;",
    ),
    (
        "an attached send revalidating every time",
        "src/tickle.c",
        "    if (!entry->recheck_due) {\n        return true;",
        "    if (false) {\n        return true;",
    ),
    (
        "the sweep not aging a yes",
        "src/tickle.c",
        "        if (entry->recheck_due || (entry->mapping == NULL && !entry->missing)) {",
        "        if (entry->recheck_due || !entry->missing) {",
    ),
    (
        "a yes kept for the no's interval",
        "src/tickle.c",
        "entry->mapping != NULL ? node->opts.segment.revalidate_ns : node->opts.segment.attach_retry_ns;",
        "entry->mapping != NULL ? node->opts.segment.attach_retry_ns : node->opts.segment.attach_retry_ns;",
    ),
    (
        "an attach keeping the no's due flag",
        "src/tickle.c",
        "    entry->recheck_due = false; // a new answer, which the next summary tick stamps (segment_recheck_sweep())\n",
        "",
    ),
    (
        "an attach keeping the no's stamp",
        "src/tickle.c",
        "(segment_recheck_sweep())\n    entry->recheck_stamped = false;\n",
        "(segment_recheck_sweep())\n",
    ),
    # 2026-10-23: a revalidation that keeps its mapping is a new answer too.
    (
        "a kept revalidation left due",
        "src/tickle.c",
        "    entry->recheck_due = false;\n    entry->recheck_stamped = false;\n    node->segment_revalidated++;",
        "    entry->recheck_stamped = false;\n    node->segment_revalidated++;",
    ),
    (
        "a kept revalidation keeping its old stamp",
        "src/tickle.c",
        "    entry->recheck_due = false;\n    entry->recheck_stamped = false;\n    node->segment_revalidated++;",
        "    entry->recheck_due = false;\n    node->segment_revalidated++;",
    ),
]


def build_and_run(control=None):
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    if unchanged(control, BIN):
        return NOT_COMPILED, ""
    run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=120, cwd=ROOT / "platform/linux")
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    backup = tempfile.mkdtemp(prefix="segment_time_limits_mutants_")
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
        control = code_digest(BIN)
        only = os.environ.get("MUTANTS", "")
        for name, rel, old, new in MUTANTS:
            if only and only not in name:
                continue
            path = ROOT / rel
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR]    {name:46s} -> anchor matched {count} times, not 1")
                failures += 1
                continue
            path.write_text(text.replace(old, new))
            verdict, output = build_and_run(control)
            restore()
            ok = verdict == "FAIL"
            failures += 0 if ok else 1
            print(f"[{label(verdict, ok):8s}] {name:46s} -> {verdict} (wanted FAIL)")
            if not ok:
                print(output[-1500:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print("mutant sweep:", "all mutants died" if failures == 0 else f"{failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
