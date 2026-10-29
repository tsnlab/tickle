#!/usr/bin/env python3
"""Mutant sweep for the segment vouch (2026-10-29): a writer attaches only to a segment discovery vouches for.

Each entry removes exactly one mechanism; tests/test_segment_vouch.c must go red for every one. A mutant that still
passes means the test is not testing what it says. Run from the repository root; it restores every file it touches,
whatever happens, and touches them after so make rebuilds from the restored source.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from mutant_digest import NOT_COMPILED, code_digest, label, unchanged

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_segment_vouch"

MUTANTS = [
    (
        "the attach asking discovery",
        "src/tickle.c",
        "    *verdict = segment_vouched(node, context_id, port, header->incarnation);",
        "    *verdict = tt_SEGMENT_ATTACHED;",
    ),
    (
        "a revalidation asking discovery",
        "src/tickle.c",
        "        segment_vouched(node, context_id, port, entry->incarnation) != tt_SEGMENT_ATTACHED) {",
        "        false) {",
    ),
    (
        "the span's bound being exclusive",
        "src/tickle.c",
        "    return offset < (uint32_t)tt_SEGMENT_VOUCH_SPAN ? tt_SEGMENT_ATTACHED : tt_SEGMENT_WRONG_OWNER;",
        "    return offset <= (uint32_t)tt_SEGMENT_VOUCH_SPAN ? tt_SEGMENT_ATTACHED : tt_SEGMENT_WRONG_OWNER;",
    ),
    (
        "the vouch being from the segment's own port",
        "src/tickle.c",
        "    if (port == 0 || node->segment_vouch_port[context_id] != port) {",
        "    if (node->segment_vouch_port[context_id] == 0) {",
    ),
    (
        "the earliest id kept modulo 2^32",
        "src/tickle.c",
        "        (int32_t)(entity_id - node->segment_vouch_entity[context_id]) < 0) {",
        "        entity_id < node->segment_vouch_entity[context_id]) {",
    ),
    (
        "a superseding announce forgetting the vouch",
        "src/tickle.c",
        "        segment_vouch_forget(node, source);\n",
        "        (void)segment_vouch_forget;\n",
    ),
    (
        "an announce's entities vouching",
        "src/tickle.c",
        "        segment_vouch_note(node, header->source, remote_entity_id, sender_port);",
        "        (void)segment_vouch_note;",
    ),
    (
        "a context's own segment checked exactly",
        "src/tickle.c",
        "            return incarnation == node->entity_id_base ? tt_SEGMENT_ATTACHED : tt_SEGMENT_WRONG_OWNER;",
        "            return tt_SEGMENT_ATTACHED;",
    ),
    (
        "a base of 0 written as the incarnation",
        "src/tickle.c",
        "    header->incarnation = node->entity_id_base;",
        "    header->incarnation = node->entity_id_base != 0 ? node->entity_id_base : 0x5eed5eedU;",
    ),
    (
        "a refused segment's ring left unwritten",
        "src/tickle.c",
        "        tt_segment_detach(header, bytes);\n        return NULL;\n    }\n    return header;",
        "        *verdict = tt_SEGMENT_ATTACHED;\n    }\n    return header;",
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
    run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=120)
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout + run.stderr


def main():
    backup = tempfile.mkdtemp(prefix="segment_vouch_mutants_")
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
