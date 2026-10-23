#!/usr/bin/env python3
"""Mutant sweep for where reliable sends put batched broadcast-only content (2026-10-23, the follow-ups of the reliable
HEARTBEAT fix; DESIGN.md section 4, "broadcast-only content goes as broadcast").

Each entry breaks one condition and names the test of tests/test_heartbeat.c that must go red for it. A mutant that
still passes means the test does not test what it says, or the condition decides nothing and should not be in the code.

Not mutated, and why:
  - request_ack's "a failed flush leaves it there" path: the mock HAL's sends do not fail in these tests; it is the
    parent's behaviour (the content rides the solicitation), kept rather than added.

Run from the repository root. Restores the file it touches, whatever happens. MUTANTS=<substring> runs only the mutants
whose name contains it (the control always runs).
"""
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = "src/tickle.c"
TEST = "obj/debug/tests/test_heartbeat"

# (name, old, new) - each `old` must occur exactly once in src/tickle.c
MUTANTS = [
    (
        "a solicitation rides the batched content (no flush first): test_publisher_request_ack_sends_batched_"
        "discovery_first",
        "    if (pub->node->tx_tail != sizeof(struct tt_Header)) {\n        flush_batched(pub->node);\n    }\n"
        "    encode_and_send_heartbeat(pub->node, pub, first_seq_no, live_peers, live_count, 0);",
        "    encode_and_send_heartbeat(pub->node, pub, first_seq_no, live_peers, live_count, 0);",
    ),
    (
        "the batched content flushed to the solicited peers: test_publisher_request_ack_sends_batched_discovery_first",
        "    if (pub->node->tx_tail != sizeof(struct tt_Header)) {\n        flush_batched(pub->node);\n    }\n"
        "    encode_and_send_heartbeat(pub->node, pub, first_seq_no, live_peers, live_count, 0);",
        "    if (pub->node->tx_tail != sizeof(struct tt_Header)) {\n"
        "        (void)flush_tx(pub->node, pub->node->tx_tail, live_peers, live_count);\n    }\n"
        "    encode_and_send_heartbeat(pub->node, pub, first_seq_no, live_peers, live_count, 0);",
    ),
    (
        "an unbatched publish rides the batched content: test_publish_sends_batched_discovery_first",
        "    if (!pub->batch) {\n        flush_pending_before_unicast(node, pub->peers);\n    }\n#if tt_LARGE_SAMPLES\n"
        "    large_before_small_publish(node, pub, data);",
        "#if tt_LARGE_SAMPLES\n    large_before_small_publish(node, pub, data);",
    ),
    (
        "the batched content before a publish flushed to its peers: test_publish_sends_batched_discovery_first",
        "        (void)flush_tx(node, node->tx_tail, NULL, 0); // a failure logs; the send then broadcasts as before",
        "        (void)flush_tx(node, node->tx_tail, peers, count); // mutant",
    ),
]


def build_and_run():
    env = dict(os.environ, TMPDIR=os.environ.get("TMPDIR", "/home/semih/tmp_build"))
    build = subprocess.run(["make", "-s", "-C", str(ROOT / "platform/linux"), TEST], capture_output=True, text=True,
                           env=env)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-2000:]
    try:
        # A mutant can loop and log without end: its output goes through `tail`, so at most the end of it is held, and
        # the run is cut off at the timeout.
        run = subprocess.run(["bash", "-c", f"ulimit -v 2097152; set -o pipefail; timeout 240 ./{TEST} 2>&1 | tail -c 4000"],
                             cwd=ROOT / "platform/linux", capture_output=True, text=True, timeout=300)
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
