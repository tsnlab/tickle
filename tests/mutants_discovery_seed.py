#!/usr/bin/env python3
"""Mutant sweep for the discovery seed (2026-11-05): an announce carries a tag and a reply echoes the tag it answers
(struct tt_AnnounceHeader.marks); the echo times the announce's round trip exactly (note_discovery_echo()), and a reliable
reader with no recovery sample of its own bases its retry interval on it (retry_interval_for()). Each mutant breaks one
decision and must turn tests/test_peer_discovery.c, test_discovery_echo_large.c, test_reliable_pubsub.c or
tests/test_estimator_trace.c red; the
control (no mutation) must pass all three, or a red mutant says nothing.

Not mutated, and why:
  - note_discovery_echo()'s first-fragment test (frag_index == 0): no test sends a fragmented announce.
  - note_large_arrival()'s stamp of tt_Context.peer_large_ns: test_large_sample's busy-writer run is its test (without the
    stamp it times a 19 ms seed on that link and fails, seen 2026-11-05), and it is too slow to run per mutant here; the
    unit tests set the field directly.
  - send_unicast_update_unless_full()'s skipped-reply test: compiled only in the large-sample build, which this sweep
    does not build.

Works on a copy: src/, include/ and tests/ go to a scratch directory, one mutation at a time is applied there, the
tests are built with gcc and run. Nothing in the tree is touched. Refuses to start (exit 2) unless every anchor occurs
exactly once in the tree's file, checked for all mutants before the control runs; every child is bounded
(tests/mutant_guard.py). Run from anywhere; exit 0 when every mutant is caught and the control passes."""
import os
import shutil
import sys
import tempfile

from mutant_guard import be_nice, check_anchors, killed_by, run_capped, usage_line

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_REL = "src/tickle.c"
TESTS = ["test_peer_discovery", "test_reliable_pubsub", "test_estimator_trace", "test_discovery_echo_large"]

MUTANTS = [
    ("control (no mutation)", None, None),
    # Which announce an echo names.
    ("echo: any slot matches whatever the tag",
     "        if (sent->sent_ns == 0 || sent->tag != echo) {\n            continue;",
     "        if (sent->sent_ns == 0) {\n            continue;"),
    ("echo: the window boundary is inclusive",
     "elapsed < DISCOVERY_ECHO_WINDOW_NS && !queued", "elapsed <= DISCOVERY_ECHO_WINDOW_NS && !queued"),
    ("echo: an announce spanning the peer's large send is timed",
     "        queued = node->peer_large_ns[source] >= sent->sent_ns;", "        queued = false;"),
    ("echo: a large send starting at the announce's own instant is not spanning",
     "        queued = node->peer_large_ns[source] >= sent->sent_ns;", "        queued = node->peer_large_ns[source] > sent->sent_ns;"),
    ("echo: any large send of the peer, however long ago, blocks",
     "        queued = node->peer_large_ns[source] >= sent->sent_ns;", "        queued = node->peer_large_ns[source] != 0;"),
    # The estimate it folds into.
    ("fold: the first sample is not taken whole",
     "        sample = 1;\n    }\n    if (*srtt_ns == 0) {\n        *srtt_ns = sample;",
     "        sample = 1;\n    }\n    if (*srtt_ns == 0) {\n        *srtt_ns = sample / 2U;"),
    ("fold: gain 1/4 instead of 1/8",
     "(int64_t)*srtt_ns) / 8));", "(int64_t)*srtt_ns) / 4));"),
    ("fold: a round trip of nothing is stored as 0 (not measured)",
     "    if (sample == 0) {\n        sample = 1;", "    if (sample == 0) {\n        sample = 0;"),
    # What is remembered, and the tags.
    ("remember: the ring never advances",
     "(node->announce_sent_next + 1U) % tt_DISCOVERY_ECHO_SLOTS", "node->announce_sent_next"),
    ("remember: the tag is not stored",
     "    slot->tag = node->announce_tag_out;", "    slot->tag = 0;"),
    ("tag: 0 is used after 255 (0 means none on the wire)",
     "    node->announce_tag = tag != 0 ? tag : 1U;", "    node->announce_tag = tag;"),
    ("tag: the echo of one reply stays on the next announce",
     "    node->announce_echo_out = 0; // that reply's, not the next announce's", "    // (mutated)"),
    ("marks: the echo in the wrong byte",
     "((uint16_t)node->announce_echo_out << 8));", "((uint16_t)node->announce_echo_out << 7));"),
    # Replying and reading.
    ("reply: carries no echo",
     "    node->announce_echo_out = echo; // the tag of the announce this answers", "    node->announce_echo_out = 0; // the tag of the announce this answers"),
    ("reply: echoes nothing from process_announce()",
     "reply_with_own_announce(node, source, sender_ip, sender_port, tag_in);", "reply_with_own_announce(node, source, sender_ip, sender_port, 0);"),
    ("read: the echo taken from the tag byte",
     "note_discovery_echo(node, source, (uint8_t)(marks >> 8));", "note_discovery_echo(node, source, (uint8_t)(marks & 0xFFU));"),
    # The use as a reader's retry interval.
    ("fallback: the seed used past the first recovery sample",
     "    if (proxy->recovery_srtt_ns == 0 && seed_ns != 0) {", "    if (seed_ns != 0) {"),
    ("fallback: the seed may shorten retry_initial",
     "        return seeded > reliable->retry_initial ? seeded : reliable->retry_initial;", "        return seeded;"),
    ("fallback: the seed of another context",
     "node->announce_rtt_ns[proxy->context_id] : 0U;", "node->announce_rtt_ns[0] : 0U;"),
    ("trace: the seed not printed",
     "            (unsigned)discovery_seed_ns(node, proxy), (unsigned long long)interval,", "            0U, (unsigned long long)interval,"),
]


def run(work, old, new):
    """Builds and runs both tests on a fresh copy with one mutation: (verdict, output)."""
    shutil.rmtree(work, ignore_errors=True)
    for d in ("src", "include", "tests"):
        shutil.copytree(os.path.join(REPO, d), os.path.join(work, d))
    if old is not None:
        target = os.path.join(work, SOURCE_REL)
        with open(target, encoding="utf-8") as f:
            text = f.read()
        with open(target, "w", encoding="utf-8") as f:
            f.write(text.replace(old, new, 1))
    outputs = []
    failed = False
    for test in TESTS:
        binary = os.path.join(work, test)
        build = run_capped(["gcc", "-O0", "-g", "-I", f"{work}/include", "-I", f"{work}/src", "-I", f"{work}/tests",
                            f'-DTICKLE_C="{work}/src/tickle.c"', "-o", binary, f"{work}/tests/{test}.c",
                            f"{work}/src/encoding.c", f"{work}/src/log.c", "-lm"], timeout=600)
        if build.returncode != 0:
            return "BUILD FAILED", build.output[-600:]
        # The linux platform directory, as make test runs them (a test may read a fixture relative to it).
        result = run_capped([binary], timeout=120, cwd=os.path.join(REPO, "platform/linux"))
        outputs.append(result.output)
        if result.timed_out or result.returncode != 0:
            failed = True
    output = "\n".join(outputs)
    lines = [line for line in output.splitlines() if "expected" in line]
    return ("failed" if failed else "passed") + (f": {lines[0]}" if lines else ""), output


def main():
    be_nice()
    check_anchors(REPO, [(name, SOURCE_REL, old, new) for name, old, new in MUTANTS if old is not None])
    scratch = tempfile.mkdtemp(prefix="mutants_discovery_seed_", dir=os.environ.get("TMPDIR"))
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
