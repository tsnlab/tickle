#!/usr/bin/env python3
"""Mutant sweep for seq_no wrap safety (tickle.c's seq_delta(), 2026-10-11).

Two families, each mutant built into its own copy of the tree (the working tree is never edited):
  serial   every `seq_delta(A, B) OP 0` in src/tickle.c, one at a time, turned back into the unsigned
           `(uint32_t)(A) OP (uint32_t)(B)` it replaced - the comparison the wrap bug was made of;
  named    the other pieces of the change, each removed: the cache ring's rebase, the reorder ring's wrap
           shift and re-pivot, the flags that replaced seq_no 0 as "none", the default-baseline cases, the
           heartbeat_last_seq_no pins.

Oracles, each built against the mutant exactly as platform/linux/Makefile builds them (debug, -O0):
  wrap     tests/test_seq_wrap.c - the rings across 2^32, and end-to-end runs from short of 2^32 against
           their control from seq_no 1;
  bitmap   tests/test_ack_bitmap_diff.c at both window sizes - the receive bitmap against a serial model,
           with a wrap scenario that sends past 2^32 while the watermark has not wrapped;
  large    tests/test_large_sample.c - 1 MB samples across 2^32, one of them starting at seq_no 0.
A mutant is killed when any oracle fails (or the mutant does not compile, reported apart). A survivor is
either equivalent within what the oracles can reach - listed with the reason in the report - or a test gap.

Before the mutants: the unmutated tree must pass every oracle (the controls), or nothing is decided.

Result on 2026-10-11 (branch claude/interesting-diffie-4cf8e1): 55 of 67 killed. The 12 survivors, by why:
  compiled out of every oracle's build: jump_ack_baseline()'s tt_RELIABLE_STATS count; the local-delivery backlog
    (tt_LOCAL_DELIVERY).
  equivalent: proxy_move_ack()'s pin of a stale heartbeat_last_seq_no - maybe_arm_acknack_retry(), which every caller
    of it runs next, pins it the same way.
  not reached at 2^32 by any oracle - large samples: the oldest retained of small and large together
    (publisher_oldest_seq_no(), reliable_cache_first_resendable_seq_no()), the end-of-sample HEARTBEAT chase
    (large_unacknowledged()), large records among small ones in a durable backlog (large_send_backlog_before()), a
    KEEP_ALL ring of tt_LARGE_RETAINED records (large_keep_all_admits()), a large sample given up across 2^32
    (drain_large_sample()), and assembly eviction with every assembly busy (large_ahead(), large_assembly_claim());
    shared memory: the segment drain's planner (plan_writer()'s lowest_seq_no).

Run from anywhere, with TMPDIR set to a roomy directory: python3 tests/mutants_seq_wrap.py [--only SUBSTRING]. Prints one line per mutant and a final
"MUTANTS killed K/N survived S invalid I". ~10 s a mutant, up to 4 min for one that hangs an oracle.
"""
import os
import pathlib
import re
import resource
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "src/tickle.c"
LARGE_DEFINES = ["-Dtt_MAX_BUFFER_LENGTH=65507", "-Dtt_LARGE_SAMPLES=1"]  # platform/linux/Makefile's LARGE_DEFINES
CFLAGS = ["-std=gnu11", "-O0", "-g", "-w", "-pthread"]
ORACLES = [  # name, test source, extra defines
    ("wrap", "tests/test_seq_wrap.c", []),
    ("bitmap", "tests/test_ack_bitmap_diff.c", []),
    ("bitmap-large", "tests/test_ack_bitmap_diff.c", LARGE_DEFINES),
    ("large", "tests/test_large_sample.c", []),
]

NAMED = [
    ("cache ring: no rebase", "    if (position >= UINT32_C(0x80000000)) {", "    if (false) {"),
    ("cache ring: origin placed only when empty",
     "    if (!cache->any_retained || seq_delta(seq_no, cache->slot_origin) < 0) {", "    if (!cache->any_retained) {"),
    ("cache ring: origin not reset when empty", "        cache->slot_origin = 1U;", "        (void)0;"),
    ("cache ring: rebase not by whole rings",
     "        cache->slot_origin += ((position - UINT32_C(0x40000000)) / depth) * depth;",
     "        cache->slot_origin += position - UINT32_C(0x40000000);"),
    ("reorder ring: no wrap shift", "        slot += reorder_shift_for(proxy, slots, salt, seq_no);",
     "        slot += 0 * reorder_shift_for(proxy, slots, salt, seq_no);"),
    ("reorder ring: no re-pivot", "        if (drift > REORDER_REPIVOT_DRIFT || drift < -REORDER_REPIVOT_DRIFT) {", "        if (false) {"),
    ("BEST_EFFORT: a new proxy's first sample checked against the default",
     "            if (!first_contact && seq_delta(ctx->seq_no, proxy->ack_seq_no) < 0) {",
     "            if (seq_delta(ctx->seq_no, proxy->ack_seq_no) < 0) {"),
    ("DURABLE: the default baseline read as a baseline",
     "    bool baseline_is_default = first_contact && sub->durable;", "    bool baseline_is_default = false;"),
    ("heartbeat_last_seq_no not pinned on a watermark move",
     "    if (seq_delta(proxy->heartbeat_last_seq_no, proxy->ack_seq_no) < 0) {\n        proxy->heartbeat_last_seq_no = ack_seq_no - 1U;",
     "    if (false) {\n        proxy->heartbeat_last_seq_no = ack_seq_no - 1U;"),
    ("heartbeat_last_seq_no not pinned per sample",
     "    if (seq_delta(proxy->heartbeat_last_seq_no, proxy->ack_seq_no) < 0) {\n        proxy->heartbeat_last_seq_no = proxy->ack_seq_no - 1U;",
     "    if (false) {\n        proxy->heartbeat_last_seq_no = proxy->ack_seq_no - 1U;"),
    ("delivered_any ignored (0 means none)", "    if (proxy->delivered_any && seq_delta(seq_no, proxy->highest_delivered) <= 0) {",
     "    if (proxy->highest_delivered != 0 && seq_delta(seq_no, proxy->highest_delivered) <= 0) {"),
    ("silent Subscriber acknowledged at 0", "            pub->peer_acks[i].ack_seq_no = publisher_unacked_from(pub);",
     "            pub->peer_acks[i].ack_seq_no = 0;"),
    ("large send idle at seq_no 0", "    while (cursor->active || large_cursor_next(node, pub)) {",
     "    while (cursor->seq_no != 0 || large_cursor_next(node, pub)) {"),
    ("durable backlog: every large record stops at UINT32_MAX",
     "    large_send_backlog_before(node, pub, 0, true, &large_next, target);",
     "    large_send_backlog_before(node, pub, UINT32_MAX, false, &large_next, target);"),
    ("cache empty when oldest is 0", "    while (cache->any_retained && seq_no - cache->oldest_seq_no + 1 > depth) {",
     "    while (cache->any_retained && cache->oldest_seq_no != 0 && seq_no - cache->oldest_seq_no + 1 > depth) {"),
]


def split_args(text, start):
    """Splits `A, B)` at text[start:] into (A, B, index past the closing parenthesis), or None."""
    depth, args, current = 0, [], start
    for i in range(start, len(text)):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            if depth == 0:
                args.append(text[current:i])
                return (args, i + 1) if len(args) == 2 else None
            depth -= 1
        elif c == "," and depth == 0:
            args.append(text[current:i])
            current = i + 1
    return None


def serial_mutants(source):
    out = []
    for m in re.finditer(r"seq_delta\(", source):
        parsed = split_args(source, m.end())
        if parsed is None:
            continue
        (a, b), end = parsed
        cmp = re.match(r"\s*(<=|>=|<|>|==|!=)\s*0\b", source[end:])
        if cmp is None:
            continue
        stop = end + cmp.end()
        line = source.count("\n", 0, m.start()) + 1
        new = f"((uint32_t)({a.strip()}) {cmp.group(1)} (uint32_t)({b.strip()}))"
        out.append((f"line {line}: seq_delta({a.strip()}, {b.strip()}) {cmp.group(1)} 0 unsigned", m.start(), stop, new))
    return out


# A mutant can loop forever printing: its output goes nowhere and its memory is capped. A first version captured the
# output, and one such mutant's log grew this script past 11 GB - the PC ran out of memory and the kernel killed
# other sessions' processes (2026-10-11).
ORACLE_MEMORY_BYTES = 2 << 30


def limit_oracle():
    resource.setrlimit(resource.RLIMIT_AS, (ORACLE_MEMORY_BYTES, ORACLE_MEMORY_BYTES))


def build_and_run(tree, work):
    """None when every oracle passes; else the name of the first that failed, or 'compile:<oracle>'."""
    for name, test, defines in ORACLES:
        binary = work / f"{name}.bin"
        build = subprocess.run(["cc", *CFLAGS, *defines, f"-I{tree}/include", f"-I{tree}/src", "-o", str(binary),
                                str(tree / test), str(tree / "src/encoding.c"), str(tree / "src/log.c"), "-lm"],
                               capture_output=True, text=True)
        if build.returncode != 0:
            return f"compile:{name}"
        try:
            run = subprocess.run([str(binary)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60,
                                 preexec_fn=limit_oracle)
        except subprocess.TimeoutExpired:
            return f"{name}(timeout)"
        if run.returncode != 0:
            return name
    return None


def main():
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    original = SRC.read_text()
    # Under $TMPDIR, as the compiler's own temporaries are: a full /tmp (a tmpfs on this PC, shared with every other
    # session) fails a compile, which would read as an invalid mutant.
    with tempfile.TemporaryDirectory(prefix="mutants_seq_wrap.", dir=os.environ.get("TMPDIR")) as tmp:
        tmp = pathlib.Path(tmp)
        tree = tmp / "tree"
        for part in ("src", "include", "tests"):
            shutil.copytree(ROOT / part, tree / part)
        target = tree / "src/tickle.c"
        control = build_and_run(tree, tmp)
        print(f"control (unmutated): {'PASS' if control is None else 'FAIL ' + control}", flush=True)
        if control is not None:
            print("MUTANTS undecided: the controls fail")
            return 2
        mutants = [(name, original.replace(old, new, 1) if original.count(old) == 1 else None)
                   for name, old, new in NAMED]
        mutants += [(name, original[:s] + new + original[e:]) for name, s, e, new in serial_mutants(original)]
        killed = survived = invalid = 0
        for name, text in mutants:
            if only is not None and only not in name:
                continue
            if text is None:
                print(f"NOT APPLICABLE  {name} (its text is not in tickle.c exactly once)", flush=True)
                invalid += 1
                continue
            target.write_text(text)
            verdict = build_and_run(tree, tmp)
            if verdict is None:
                survived += 1
                print(f"SURVIVED  {name}", flush=True)
            elif verdict.startswith("compile:"):
                invalid += 1
                print(f"INVALID   {name} ({verdict})", flush=True)
            else:
                killed += 1
                print(f"killed    {name} by {verdict}", flush=True)
        target.write_text(original)
        print(f"MUTANTS killed {killed}/{killed + survived} survived {survived} invalid {invalid}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
