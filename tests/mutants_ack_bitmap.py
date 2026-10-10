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

Not swept: proxy_clear_received() at a WriterProxy's creation. The slot's count is already 0 there - forgetting a
writer (forget_writer_proxies_for_endpoint()) clears it with the bitmap - so dropping that reset is an equivalent mutant. Nor jump_ack_baseline()'s proxy_clear_received(): the
advance_ack_seq_no() right after it shifts and trims, which brings a stale count back to 0 before anything reads it
(swept on 2026-10-10, it survived both oracles, as an equivalent mutant must).

Four kills are of the count's exactness alone, which only the unit oracle checks: the clear's reset and the three
trims. Without them the count stays too high, which costs work but changes no output, so the trace diff passes them.

Run from anywhere: python3 tests/mutants_ack_bitmap.py [BASE]. Restores src/tickle.c whatever happens.
"""
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "platform/linux/obj/debug/tests/test_ack_bitmap_diff"
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


def run_unit():
    build = subprocess.run(
        ["make", "-s", "-C", str(ROOT / "platform/linux"), str(BIN.relative_to(ROOT / "platform/linux"))],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        return "BUILD_FAILED", build.stderr[-1500:]
    try:
        run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=30)  # ~2 s unmutated
    except subprocess.TimeoutExpired:
        return "HANG", ""
    if run.returncode < 0:
        return "CRASH", run.stderr[-1000:]
    return ("PASS" if run.returncode == 0 else "FAIL"), run.stderr[-1000:]


def run_diff():
    try:
        run = subprocess.run([str(ROOT / "tests/ack_bitmap_differential.sh"), BASE], capture_output=True, text=True,
                             timeout=300)
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
    try:
        unit, unit_out = run_unit()
        diff, diff_out = run_diff()
        print(f"[control ] unmutated                                       unit {unit:6s} diff {diff}")
        if unit != "PASS" or diff != "PASS":
            print(unit_out, diff_out)
            print("control did not pass; the sweep cannot decide anything")
            return 1
        for name, old, new in MUTANTS:
            path = ROOT / SRC
            text = path.read_text()
            count = text.count(old)
            if count != 1:
                print(f"[ERROR   ] {name:55s} anchor matched {count} times, not 1")
                problems += 1
                continue
            path.write_text(text.replace(old, new))
            path.touch()
            unit, unit_out = run_unit()
            diff, diff_out = run_diff()
            restore()
            dead = unit in ("FAIL", "CRASH", "HANG") or diff == "FAIL"
            undecided = unit == "BUILD_FAILED" or diff in ("UNDECIDED", "NO_VERDICT", "HANG")
            killed += 1 if dead else 0
            problems += 0 if dead else 1
            tag = "killed" if dead else ("UNDECID" if undecided else "SURVIVED")
            print(f"[{tag:8s}] {name:55s} unit {unit:6s} diff {diff}", flush=True)
            if not dead:
                print(unit_out[-800:], diff_out[-800:])
    finally:
        restore()
        shutil.rmtree(backup, ignore_errors=True)
    print(f"mutant sweep: {killed}/{len(MUTANTS)} killed", "" if problems == 0 else f"- {problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
