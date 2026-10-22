#!/usr/bin/env python3
"""Mutant sweep for the discovery timing a summary carries (the user's decision of 2026-10-22, network-profiles item C:
tt_HeartbeatHeader's three reserved bytes, zero = the default). Each mutant breaks one decision and must turn
tests/test_discovery_timing.c or tests/test_init_options_discovery.c red; the control (no mutation) must pass both, or
a red mutant says nothing.

Not mutated, and why:
  - process_heartbeat()'s placement of note_peer_timing() after the liveliness-flag return: a liveliness assertion is
    never sent for the discovery endpoint, so no datagram reaches the other order.
  - the default wire bytes as a whole: tests/wire_identity_discovery_timing.sh compares a capture with the parent's.

Works on a copy: src/, include/ and tests/ go to a scratch directory, one mutation at a time is applied there, the two
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
TESTS = ["test_discovery_timing", "test_init_options_discovery"]

MUTANTS = [
    ("control (no mutation)", None, None),
    # The sender.
    ("encode: the default sent as a code, not 0",
     "    if (interval_ns == (uint64_t)tt_CONTEXT_UPDATE_INTERVAL) {\n        return 0;\n    }\n    for (uint32_t unit",
     "    for (uint32_t unit"),
    ("encode: rounded down", "(interval_ns + summary_interval_units[unit] - 1U) / summary_interval_units[unit];",
     "interval_ns / summary_interval_units[unit];"),
    ("encode: 8191 not a count", "        if (count <= tt_SUMMARY_INTERVAL_MANTISSA_MAX) {",
     "        if (count < tt_SUMMARY_INTERVAL_MANTISSA_MAX) {"),
    ("encode: past every unit sends the default", "    return UINT16_MAX;\n}", "    return 0;\n}"),
    ("summary: bytes least significant first",
     "    summary->summary_interval[0] = (uint8_t)(interval >> 8);\n    summary->summary_interval[1] = (uint8_t)interval;",
     "    summary->summary_interval[0] = (uint8_t)interval;\n    summary->summary_interval[1] = (uint8_t)(interval >> 8);"),
    ("summary: the default threshold sent as itself",
     "    summary->summary_miss_threshold = miss == (uint64_t)tt_LIVELINESS_MISS_THRESHOLD ? 0 : (uint8_t)miss;",
     "    summary->summary_miss_threshold = (uint8_t)miss;"),
    ("summary: no threshold sent",
     "    summary->summary_miss_threshold = miss == (uint64_t)tt_LIVELINESS_MISS_THRESHOLD ? 0 : (uint8_t)miss;",
     "    summary->summary_miss_threshold = 0;"),
    ("summary: no interval sent",
     "    uint16_t interval = summary_interval_encode(discovery_update_interval_ns(node));",
     "    uint16_t interval = 0;"),
    # The receiver.
    ("decode: a code of no units read as 0 ns, not the default",
     "    if (count == 0) {\n        return (uint64_t)tt_CONTEXT_UPDATE_INTERVAL;\n    }\n    uint64_t interval =",
     "    uint64_t interval ="),
    ("decode: not held to a day",
     "    return interval < tt_DISCOVERY_MAX_DURATION_NS ? interval : tt_DISCOVERY_MAX_DURATION_NS;",
     "    return interval;"),
    ("receive: the summary's timing not taken",
     "        note_peer_timing(node, header->source, heartbeat_header);\n",
     ""),
    ("receive: bytes read least significant first",
     "(uint16_t)(((uint32_t)summary->summary_interval[0] << 8) | summary->summary_interval[1]);",
     "(uint16_t)(((uint32_t)summary->summary_interval[1] << 8) | summary->summary_interval[0]);"),
    ("receive: the advertised threshold ignored",
     "        summary_interval_decode(code), miss != 0 ? (uint64_t)miss : (uint64_t)tt_LIVELINESS_MISS_THRESHOLD);",
     "        summary_interval_decode(code), (uint64_t)tt_LIVELINESS_MISS_THRESHOLD);"),
    ("receive: a zero threshold is the receiver's own",
     "        summary_interval_decode(code), miss != 0 ? (uint64_t)miss : (uint64_t)tt_LIVELINESS_MISS_THRESHOLD);",
     "        summary_interval_decode(code), miss != 0 ? (uint64_t)miss : discovery_miss_threshold(node));"),
    ("receive: a zero interval is the receiver's own",
     "    node->peer_silence_ns[source] = discovery_silence_of(\n        summary_interval_decode(code),",
     "    node->peer_silence_ns[source] = discovery_silence_of(\n"
     "        code != 0 ? summary_interval_decode(code) : discovery_update_interval_ns(node),"),
    ("judge: every peer by our own silence (the netns control's switch)",
     "        uint64_t silence = node->peer_silence_ns[i] != 0 ? node->peer_silence_ns[i] : own_silence;",
     "        uint64_t silence = own_silence;"),
    ("judge: a peer with no summary yet by the default, not our own",
     "        uint64_t silence = node->peer_silence_ns[i] != 0 ? node->peer_silence_ns[i] : own_silence;",
     "        uint64_t silence = node->peer_silence_ns[i] != 0 ? node->peer_silence_ns[i] : tt_LIVELINESS_SILENCE_NS;"),
    ("death: the value kept", "    node->peer_silence_ns[source] = 0; // its next summary says again\n", ""),
    ("creation: the values not cleared", "        node->peer_silence_ns[i] = 0;\n", ""),
    # The creation line.
    ("the interop line back at WARNING",
     "        TT_LOG_INFO(\"tt_InitOptions.discovery: update_interval_ns",
     "        TT_LOG_WARNING(\"tt_InitOptions.discovery: update_interval_ns"),
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
    scratch = tempfile.mkdtemp(prefix="mutants_discovery_timing_", dir=os.environ.get("TMPDIR"))
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
