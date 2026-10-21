#!/usr/bin/env python3
"""Mutant sweep for QoS plan D1, D8, D9 (tt_VERSION 14): the lifespan and depth in the announce, and a reader held to
min(writer, reader). Each mutant breaks one piece and must turn tests/test_lifespan_announce.c red; the control (no
mutation) must pass, or a red mutant says nothing.

Works on a copy: src/, include/ and tests/ go to a scratch directory, one mutation at a time is applied there, the test
is built with gcc and run. Nothing in the tree is touched. Refuses to start (exit 2) unless every anchor occurs exactly
once in the tree's file, checked for all mutants before the control runs; every child is bounded (tests/mutant_guard.py:
2 GB of address space, a deadline, at most 256 KB of its output kept). Importing this file runs nothing. Run from
anywhere; exit 0 when every mutant is caught and the control passes."""
import os
import shutil
import sys
import tempfile

from mutant_guard import be_nice, check_anchors, killed_by, run_capped, usage_line

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER_REL = "include/tickle/tickle.h"
SOURCE_REL = "src/tickle.c"

MUTANTS = [
    ("control (no mutation)", None, None),
    ("D1 off: the reader's own only", "    return lifespan_min(own, writer);\n}", "    return own;\n}"),
    ("the writer's only", "    return lifespan_min(own, writer);\n}", "    return writer != 0 ? writer : own;\n}"),
    ("max for min", "    return b != 0 && b < a ? b : a;", "    return b != 0 && b > a ? b : a;"),
    ("table drops the lifespan", "    slot->lifespan_duration_ns = lifespan_duration_ns; // tt_VERSION 14",
     "    slot->lifespan_duration_ns = 0; // tt_VERSION 14"),
    ("table drops the depth", "    slot->history_depth = history_depth;", "    slot->history_depth = 0;"),
    ("announce sends no lifespan",
     "        update_entity->lifespan_duration_ns = endpoint_lifespan_duration_ns(endpoint); // tt_VERSION 14",
     "        update_entity->lifespan_duration_ns = 0; // tt_VERSION 14"),
    ("announce sends no depth", "        update_entity->history_depth = endpoint_history_depth(endpoint);",
     "        update_entity->history_depth = 0;"),
    ("no flag from an announce",
     "            for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint_id, note_writer_lifespan, NULL);",
     "            (void)note_writer_lifespan;"),
    ("no flag from the table at creation", "                                    entity->lifespan_duration_ns != 0;",
     "                                    false;"),
    ("no flag for a local writer created later",
     "            for_each_endpoint(node, tt_KIND_TOPIC_SUBSCRIBER, endpoint->id, note_writer_lifespan, NULL);",
     "            (void)0;"),
    ("no Subscriber KEEP_ALL bit", "                         (keep_all ? tt_UPDATE_QOS_KEEP_ALL : 0));\n    }\n    default:",
     "                         0);\n    }\n    default:"),
    ("version left at 13", "#define tt_VERSION 14", "#define tt_VERSION 13"),
]


def target_of(old):
    return HEADER_REL if old.startswith("#define tt_VERSION") else SOURCE_REL


def run(work, old, new):
    """Builds and runs the test on a fresh copy with one mutation: (verdict, output)."""
    shutil.rmtree(work, ignore_errors=True)
    for d in ("src", "include", "tests"):
        shutil.copytree(os.path.join(REPO, d), os.path.join(work, d))
    if old is not None:
        target = os.path.join(work, target_of(old))
        with open(target, encoding="utf-8") as f:
            text = f.read()
        with open(target, "w", encoding="utf-8") as f:
            f.write(text.replace(old, new, 1))
    binary = os.path.join(work, "t")
    build = run_capped(["gcc", "-O0", "-g", "-I", f"{work}/include", "-I", f"{work}/src", "-I", f"{work}/tests",
                        f'-DVERSION_FIXTURES_DIR="{work}/tests"', "-o", binary,
                        f"{work}/tests/test_lifespan_announce.c", f"{work}/src/encoding.c", f"{work}/src/log.c",
                        "-lm"], timeout=600)
    if build.returncode != 0:
        return "BUILD FAILED", build.output[-300:]
    result = run_capped([binary], timeout=120)
    if result.timed_out:
        return "failed (timeout)", result.output
    lines = [line for line in result.output.splitlines() if "expected" in line]
    return ("passed" if result.returncode == 0 else "failed") + (f": {lines[0]}" if lines else ""), result.output


def main():
    be_nice()
    check_anchors(REPO, [(name, target_of(old), old, new) for name, old, new in MUTANTS if old is not None])
    scratch = tempfile.mkdtemp(prefix="mutants_lifespan_", dir=os.environ.get("TMPDIR"))
    work = os.path.join(scratch, "tree")
    failures = 0
    try:
        for name, old, new in MUTANTS:
            verdict, output = run(work, old, new)
            ok = verdict.startswith("passed") if old is None else verdict.startswith("failed")
            failures += 0 if ok else 1
            print(f"{'PASS' if ok else 'FAIL'}  {name}: {verdict}", flush=True)
            if old is None and not ok:
                print(output[-2000:])
                print("control did not pass; the sweep cannot decide anything")
                return 1
            if old is not None and ok:
                print(f"      killed by: {', '.join(killed_by(output, REPO)) or '(no failing check printed)'}")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print(usage_line())
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
