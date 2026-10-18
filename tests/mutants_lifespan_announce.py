#!/usr/bin/env python3
"""Mutant sweep for QoS plan D1, D8, D9 (tt_VERSION 14): the lifespan and depth in the announce, and a reader held to
min(writer, reader). Each mutant breaks one piece and must turn tests/test_lifespan_announce.c red; the control (no
mutation) must pass, or a red mutant says nothing.

Works on a copy: src/, include/ and tests/ go to a scratch directory, one mutation at a time is applied there, the test
is built with gcc and run under a timeout and a 2 GB address-space limit. Nothing in the tree is touched. Run from
anywhere; exit 0 when every mutant is caught and the control passes."""
import os
import resource
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(tempfile.mkdtemp(prefix="mutants_lifespan_"), "tree")

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


def limit():
    resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))


def run(name, old, new):
    shutil.rmtree(WORK, ignore_errors=True)
    for d in ("src", "include", "tests"):
        shutil.copytree(os.path.join(REPO, d), os.path.join(WORK, d))
    if old is not None:
        target = os.path.join(WORK, "include/tickle/tickle.h" if old.startswith("#define tt_VERSION") else "src/tickle.c")
        text = open(target).read()
        if text.count(old) != 1:
            return f"NOT APPLIED ({text.count(old)} matches)"
        open(target, "w").write(text.replace(old, new))
    binary = os.path.join(WORK, "t")
    build = subprocess.run(["gcc", "-O0", "-g", "-I", f"{WORK}/include", "-I", f"{WORK}/src", "-I", f"{WORK}/tests",
                            f'-DVERSION_FIXTURES_DIR="{WORK}/tests"', "-o", binary,
                            f"{WORK}/tests/test_lifespan_announce.c", f"{WORK}/src/encoding.c", f"{WORK}/src/log.c",
                            "-lm"], capture_output=True, text=True)
    if build.returncode != 0:
        return "BUILD FAILED: " + build.stderr[-300:]
    try:
        r = subprocess.run([binary], capture_output=True, text=True, timeout=120, preexec_fn=limit)
    except subprocess.TimeoutExpired:
        return "failed (timeout)"
    lines = [l for l in (r.stdout + r.stderr).splitlines() if "expected" in l]
    return ("passed" if r.returncode == 0 else "failed") + (f": {lines[0]}" if lines else "")


failures = 0
for name, old, new in MUTANTS:
    verdict = run(name, old, new)
    ok = verdict.startswith("passed") if old is None else verdict.startswith("failed")
    failures += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {verdict}")
shutil.rmtree(os.path.dirname(WORK), ignore_errors=True)
sys.exit(1 if failures else 0)
