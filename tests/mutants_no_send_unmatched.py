#!/usr/bin/env python3
"""Mutant sweep for "a Publisher with no matched reader sends nothing" (DESIGN.md section 4, 2026-10-15).

Each entry breaks one condition of the rule and must turn tests/test_no_send_unmatched.c red. A mutant that still
passes means the test does not test what it says, or the condition decides nothing and should not be in the code.

Not mutated, and why:
  - publish_large_instead()'s flush of what was batched ahead: it carries other content (a pending announce) that was
    going to be broadcast anyway; with no matched reader no match Heartbeat is put there.
  - large_send_end_heartbeat() has no gate of its own: large_end_heartbeat() returns first with no matched reader, and
    the ACK chase it arms runs only while a matched reader has not acknowledged (large_unacknowledged()).

Run from the repository root. Restores the file it touches, whatever happens. MUTANTS=<substring> runs only the mutants
whose name contains it (the control always runs).
"""
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = "src/tickle.c"
TEST = "obj/debug/tests/test_no_send_unmatched"

# (name, old, new)
MUTANTS = [
    (
        "rule: publisher_unmatched() never true",
        "    return pub->peers[0].context_id == tt_CONTEXT_ID_INVALID;\n}\n\nstatic void count_unmatched_sample(",
        "    return false;\n}\n\nstatic void count_unmatched_sample(",
    ),
    (
        "rule: the second slot read as the answer (one matched reader reads as none)",
        "    return pub->peers[0].context_id == tt_CONTEXT_ID_INVALID;\n}\n\nstatic void count_unmatched_sample(",
        "    return pub->peers[1].context_id == tt_CONTEXT_ID_INVALID;\n}\n\nstatic void count_unmatched_sample(",
    ),
    (
        "staging: the unmatched return removed",
        "    if (publisher_unmatched(pub)) {\n        // No matched reader: encoded, and retained above",
        "    if (false) {\n        // No matched reader: encoded, and retained above",
    ),
    (
        "staging: an unmatched sample takes no seq_no",
        "        count_unmatched_sample(node, pub);\n        pub->seq_no += seq_span;\n",
        "        count_unmatched_sample(node, pub);\n",
    ),
    (
        "zero-copy: the unmatched return removed",
        "    if (peer_count == 0) {\n        // No matched reader: nothing is sent (publisher_unmatched()).",
        "    if (false) {\n        // No matched reader: nothing is sent (publisher_unmatched()).",
    ),
    (
        "zero-copy: an unmatched sample takes no seq_no",
        "        pub->seq_no += sizeof(framing) + body_len > FRAG_WHOLE_DATA_LIMIT ? frag_count_for(body_len) : 1U;\n",
        "",
    ),
    (
        "large: the empty destination set removed",
        "    if (count == 0) {\n        // No matched reader: no destination,",
        "    if (false) {\n        // No matched reader: no destination,",
    ),
    (
        "large: the end-of-sample HEARTBEAT to nobody",
        "    if (!pub->reliable || publisher_unmatched(pub)) {\n        return; // a sample that went to nobody",
        "    if (!pub->reliable) {\n        return; // a sample that went to nobody",
    ),
    (
        "large: the ranged encode left to a send that never comes",
        "if (pub->topic->data_encode_range == NULL || !sending_now || publisher_unmatched(pub)) {",
        "if (pub->topic->data_encode_range == NULL || !sending_now) {",
    ),
    (
        "heartbeat: the periodic HEARTBEAT to nobody",
        "    if (!publisher_unmatched(pub) && publisher_oldest_seq_no(pub, &first_seq_no)) {",
        "    if (publisher_oldest_seq_no(pub, &first_seq_no)) {",
    ),
    (
        "heartbeat: an owed match HEARTBEAT to nobody",
        "    if (publisher_unmatched(pub)) {\n        return; // with no matched reader nothing is sent (publisher_unmatched()); it stays owed\n    }\n",
        "",
    ),
    (
        "count: dropped and kept swapped",
        "    if (pub->reliable_cache != NULL) {\n        node->tx_unmatched_kept++;",
        "    if (pub->reliable_cache == NULL) {\n        node->tx_unmatched_kept++;",
    ),
]


def build_and_run():
    env = dict(os.environ, TMPDIR=os.environ.get("TMPDIR", "/home/semih/tmp_build"))
    build = subprocess.run(["make", "-s", "-C", str(ROOT / "platform/linux"), TEST], capture_output=True, text=True,
                           env=env)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    try:
        run = subprocess.run(["bash", "-c", f"ulimit -v 2097152; exec ./{TEST}"], cwd=ROOT / "platform/linux",
                             capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return "FAILED", "timeout"
    return ("PASSED" if run.returncode == 0 else "FAILED"), run.stdout[-1500:]


def main():
    path = ROOT / SRC
    original = path.read_text()
    only = os.environ.get("MUTANTS", "")
    survivors = []
    try:
        verdict, out = build_and_run()
        print(f"control: {verdict}")
        if verdict != "PASSED":
            print(out)
            return 2
        for name, old, new in MUTANTS:
            if only and only not in name:
                continue
            if original.count(old) != 1:
                print(f"{name}: the text to mutate occurs {original.count(old)} times - the sweep is stale")
                return 2
            path.write_text(original.replace(old, new))  # a new mtime: make rebuilds the test
            verdict, out = build_and_run()
            killed = verdict == "FAILED"
            print(f"{name}: {'killed' if killed else verdict}")
            if not killed:
                survivors.append(name)
    finally:
        path.write_text(original)
    # The restored file has a new mtime, so the next build is of the real code again.
    if survivors:
        print(f"{len(survivors)} mutant(s) survived: {survivors}")
        return 1
    print("every mutant killed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
