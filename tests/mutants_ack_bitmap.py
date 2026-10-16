#!/usr/bin/env python3
"""Mutant sweep for the reliable receive bitmap's words-in-use count (tt_WriterProxy.received_words, tickle.c's
proxy_*_received() helpers, 2026-10-10).

Each entry breaks exactly one condition of the fast path - the count raised on a set, lowered on a shift or a
clear, the shift and the highest-bit scan limited to it - and must be killed by at least one of two oracles:
  unit  tests/test_ack_bitmap_diff.c: the bitmap and watermark against a naive model, the highest bit, and the
        count's exactness, after every step;
  diff  tests/ack_bitmap_differential.sh: the same sequences traced on the base tickle.c and on the mutant, with
        every ACKNACK hashed - the old-vs-new equivalence itself.
A mutant both oracles pass means the condition decides nothing, or the tests do not test it.

Both oracles run at both window sizes - the default build (4,096 bits) and -Dtt_LARGE_SAMPLES=1 (8,192 bits, as
rmw_tickle builds core; platform/linux/Makefile's LARGE_DEFINES) - and a mutant counts as killed only when it dies at
each size on its own. Killed at one size alone would leave the other window's fast path with no test shown to catch it.

Not swept: proxy_clear_received() at a WriterProxy's creation. The slot's count is already 0 there - forgetting a
writer (forget_writer_proxies_for_endpoint()) clears it with the bitmap - so dropping that reset is an equivalent mutant. Nor jump_ack_baseline()'s proxy_clear_received(): the
advance_ack_seq_no() right after it shifts and trims, which brings a stale count back to 0 before anything reads it
(swept on 2026-10-10, it survived both oracles, as an equivalent mutant must).

Four kills are of the count's exactness alone, which only the unit oracle checks: the clear's reset and the three
trims. Without them the count stays too high, which costs work but changes no output, so the trace diff passes them.

Run from anywhere: python3 tests/mutants_ack_bitmap.py [BASE]. Restores src/tickle.c whatever happens. ~2 min per
size for the controls plus 25-60 s per mutant and size. MUTANTS=<substring> runs only the mutants whose name contains it.

A mutant whose unit binary at a size is the control's, debug sections aside (tests/mutant_digest.py), is DEAD
(NOT_COMPILED) at that size: it edited code that size's build leaves out, and the differential, which compiles the same
tickle.c with the same defines (at -O1), is not run for it. Like any size it does not die at, that counts as a problem.
"""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

from mutant_digest import NOT_COMPILED, code_digest, unchanged

ROOT = pathlib.Path(__file__).resolve().parents[1]
LINUX = ROOT / "platform/linux"
# size -> (unit test binary, ack_bitmap_differential.sh's flags). make test builds and runs both binaries.
SIZES = {
    "default": (LINUX / "obj/debug/tests/test_ack_bitmap_diff", []),
    "large": (LINUX / "obj/debug/tests-large/test_ack_bitmap_diff", ["--large"]),
}
SRC = "src/tickle.c"
BASE = sys.argv[1] if len(sys.argv) > 1 else "origin/main"

MUTANTS = [
    (
        "a set raises the count",
        "    if (word >= proxy->received_words) {\n        proxy->received_words = (uint16_t)(word + 1U);\n    }\n",
        "",
    ),
    (
        "  ... to include the word set",
        "        proxy->received_words = (uint16_t)(word + 1U);",
        "        proxy->received_words = (uint16_t)(word);",
    ),
    (
        "  ... only ever upward",
        "    if (word >= proxy->received_words) {",
        "    if (true) {",
    ),
    (
        "a clear resets the count",
        "    bitmap_clear(proxy->received_bitmap, proxy_words(proxy));\n    proxy->received_words = 0;\n",
        "    bitmap_clear(proxy->received_bitmap, proxy_words(proxy));\n",
    ),
    (
        "the trim stops at a nonzero word",
        "    while (words != 0 && proxy->received_bitmap[words - 1] == 0) {",
        "    while (words != 0 && proxy->received_bitmap[words - 1] <= 1) {",
    ),
    (
        "the trim is kept",
        "        words--;\n    }\n    proxy->received_words = words;\n",
        "        words--;\n    }\n",
    ),
    (
        "the shift is skipped only when nothing is set",
        "    if (proxy->received_words == 0) {\n        return; // nothing set: the shift would move zeros",
        "    if (proxy->received_words <= 1) {\n        return; // nothing set: the shift would move zeros",
    ),
    (
        "the shift covers every word in use",
        "    bitmap_shift_right_one(proxy->received_bitmap, proxy->received_words);",
        "    bitmap_shift_right_one(proxy->received_bitmap,\n"
        "                           (uint16_t)(proxy->received_words > 1 ? proxy->received_words - 1 : 1));",
    ),
    (
        "the shift is followed by a trim",
        "    bitmap_shift_right_one(proxy->received_bitmap, proxy->received_words);\n    proxy_trim_received(proxy);\n",
        "    bitmap_shift_right_one(proxy->received_bitmap, proxy->received_words);\n",
    ),
    (
        "the highest-bit scan covers every word in use",
        "    return bitmap_highest_bit(proxy->received_bitmap, proxy->received_words);",
        "    return bitmap_highest_bit(proxy->received_bitmap,\n"
        "                              (uint16_t)(proxy->received_words > 1 ? proxy->received_words - 1 : 0));",
    ),
    (
        "a heartbeat's skip trims",
        "        bitmap_shift_right(proxy->received_bitmap, proxy_words(proxy), skipped);\n        proxy_trim_received(proxy);\n",
        "        bitmap_shift_right(proxy->received_bitmap, proxy_words(proxy), skipped);\n",
    ),
    (
        "the in-window DATA path sets through the helper",
        "    proxy_mark_received(proxy, offset);\n    return true;",
        "    bitmap_set_bit(proxy->received_bitmap, offset);\n    return true;",
    ),
    (
        "a span sets through the helper",
        "            proxy_mark_received(proxy, (uint32_t)offset);\n            absorbed++;",
        "            bitmap_set_bit(proxy->received_bitmap, (uint32_t)offset);\n            absorbed++;",
    ),
]


def run_unit(size, control=None):
    binary = SIZES[size][0]
    build = subprocess.run(["make", "-s", "-C", str(LINUX), str(binary.relative_to(LINUX))], capture_output=True,
                           text=True)
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-1500:]
    if unchanged(control, binary):
        return NOT_COMPILED, ""
    try:
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)  # 3-5 s unmutated
    except subprocess.TimeoutExpired:
        return "HANG", ""
    if run.returncode < 0:
        return "CRASH", run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stderr[-1000:]


def run_diff(size):
    try:
        run = subprocess.run([str(ROOT / "tests/ack_bitmap_differential.sh"), *SIZES[size][1], BASE],
                             capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return "HANG", ""
    # The script's own last line on stdout, not its exit status: "DIFF_EXIT=2" is "could not decide", not a kill.
    # stdout alone: a mutant's test binary writes its failures to stderr, which would otherwise come last.
    last = run.stdout.strip().splitlines()[-1] if run.stdout.strip() else ""
    verdict = {"DIFF_EXIT=0": "PASS", "DIFF_EXIT=1": "FAIL", "DIFF_EXIT=2": "UNDECIDED"}.get(last, "NO_VERDICT")
    return verdict, (run.stdout + run.stderr)[-800:]


def main():
    backup = tempfile.mkdtemp(prefix="ack_bitmap_mutants_")
    saved = pathlib.Path(backup) / "tickle.c"
    shutil.copy2(ROOT / SRC, saved)

    def restore():
        # copy() then touch(): copy2 would keep the old mtime and make would reuse the mutant's objects.
        shutil.copy(saved, ROOT / SRC)
        (ROOT / SRC).touch()

    signal.signal(signal.SIGTERM, lambda _s, _f: sys.exit(1))
    problems = 0
    killed = 0
    digests = {}
    only = os.environ.get("MUTANTS", "")
    try:
        for size in SIZES:
            unit, unit_out = run_unit(size)
            diff, diff_out = run_diff(size)
            print(f"[control ] unmutated, {size:7s}                              unit {unit:6s} diff {diff}", flush=True)
            if unit != "PASS" or diff != "PASS":
                print(unit_out, diff_out)
                print(f"control did not pass at the {size} size; the sweep cannot decide anything")
                return 1
            digests[size] = code_digest(SIZES[size][0])
        for name, old, new in MUTANTS:
            if only and only not in name:
                continue
            path = ROOT / SRC
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR   ] {name:55s} anchor matched {count} times, not 1")
                problems += 1
                continue
            path.write_text(text.replace(old, new))
            path.touch()
            dead_at = []
            for size in SIZES:
                unit, unit_out = run_unit(size, digests[size])
                # A mutant that hangs or crashes the unit test does the same to the traced build, and the differential
                # would only wait out its own 120 s timeout to say so: ~6 min per such mutant on 2026-10-11. One whose
                # unit binary is the control's changed nothing this size compiles, so the trace cannot differ either.
                diff, diff_out = ("SKIPPED", "") if unit in ("HANG", "CRASH", NOT_COMPILED) else run_diff(size)
                dead = unit in ("FAIL", "CRASH", "HANG") or diff == "FAIL"
                undecided = unit == "BUILD_FAILED" or diff in ("UNDECIDED", "NO_VERDICT", "HANG")
                dead_at.append(dead)
                tag = "killed" if dead else ("DEAD" if unit == NOT_COMPILED else "UNDECID" if undecided else "SURVIVED")
                print(f"[{tag:8s}] {name:46s} {size:7s} unit {unit:6s} diff {diff}", flush=True)
                if not dead:
                    print(unit_out[-800:], diff_out[-800:])
            restore()
            killed += 1 if all(dead_at) else 0
            problems += 0 if all(dead_at) else 1
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print(f"mutant sweep: {killed}/{len(MUTANTS)} killed at both sizes",
          "" if problems == 0 else f"- {problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
