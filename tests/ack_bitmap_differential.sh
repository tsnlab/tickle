#!/usr/bin/env bash
# Old-vs-new differential for the reliable receive bitmap (tests/test_ack_bitmap_diff.c).
#
# Builds test_ack_bitmap_diff.c twice in trace mode - against src/ and include/ as they were at BASE
# (default origin/main) and as they are in this working tree - runs both on the same deterministic
# sequences, and diffs the traces: per step the watermark, retry state, a hash of the whole bitmap and
# a hash of every datagram sent (the ACKNACKs). Identical traces are the claim; any line that differs
# is a behaviour change.
#
# It refuses to call two traces equal unless both are complete (one line per step, the count the
# scenario table adds up to) and the trace saw ACKNACKs at all, so an empty or truncated run cannot
# read as "no difference".
#
# Usage: tests/ack_bitmap_differential.sh [BASE]   (from anywhere; prints DIFF_EXIT=0 on equal traces)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BASE=${1:-origin/main}
CC=${CC:-cc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ack_bitmap_diff.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

build() { # build <tree-root> <out>
    local tree=$1 out=$2
    mkdir -p "$tree/tests"
    cp "$ROOT/tests/test_ack_bitmap_diff.c" "$ROOT/tests/test_common.h" "$ROOT/tests/test_mock.h" "$tree/tests/"
    "$CC" -O1 -g -w -DACK_DIFF_TRACE_ONLY -I"$tree/include" -I"$tree/src" -o "$out" \
        "$tree/tests/test_ack_bitmap_diff.c" "$tree/src/encoding.c" "$tree/src/log.c" -lm
}

mkdir -p "$WORK/old" "$WORK/new"
git -C "$ROOT" archive "$BASE" src include | tar -x -C "$WORK/old"
cp -r "$ROOT/src" "$ROOT/include" "$WORK/new/"
build "$WORK/old" "$WORK/old.bin"
build "$WORK/new" "$WORK/new.bin"

# A failed positive control exits 1 and still leaves a whole trace, which is what is compared. A crash or a hang
# (timeout: 124, signals: 128+) is a behaviour of its own, decided below.
old_status=0
new_status=0
timeout 120 "$WORK/old.bin" --trace > "$WORK/old.trace" || old_status=$?
timeout 120 "$WORK/new.bin" --trace > "$WORK/new.trace" || new_status=$?
echo "exit status: base $old_status, working tree $new_status"
if [ "$old_status" -gt 1 ]; then
    echo "the base build did not complete - cannot decide"
    echo "DIFF_EXIT=2"
    exit 2
fi
if [ "$new_status" -gt 1 ]; then
    echo "the working tree's build did not complete where the base did (status $new_status)"
    echo "DIFF_EXIT=1"
    exit 1
fi

expected=$(awk '/^static const struct scenario scenarios/ {on=1; next} on && /^};/ {on=0}
    on && /^ *\{"/ {split($0, f, ","); steps += f[3]} END {print steps * 6}' "$ROOT/tests/test_ack_bitmap_diff.c")
old_lines=$(grep -c ' ret=' "$WORK/old.trace" || true)
new_lines=$(grep -c ' ret=' "$WORK/new.trace" || true)
old_sends=$(awk '{for (i = 1; i <= NF; i++) if ($i ~ /^sends=/) {split($i, s, "="); n += s[2]}} END {print n + 0}' "$WORK/old.trace")
echo "base $BASE: $old_lines steps, $old_sends datagrams sent; working tree: $new_lines steps; expected $expected steps"
if [ "$old_lines" != "$expected" ] || [ "$new_lines" != "$expected" ] || [ "$old_sends" -eq 0 ]; then
    echo "incomplete trace - cannot decide"
    echo "DIFF_EXIT=2"
    exit 2
fi
if diff -q "$WORK/old.trace" "$WORK/new.trace" > /dev/null; then
    echo "traces identical"
    echo "DIFF_EXIT=0"
    exit 0
fi
# diff exits 1 on a difference, which pipefail would turn into this script's death before its verdict line.
diff "$WORK/old.trace" "$WORK/new.trace" > "$WORK/trace.diff" || true
head -20 "$WORK/trace.diff"
echo "traces differ: $(grep -c '^<' "$WORK/trace.diff" || true) step(s)"
echo "DIFF_EXIT=1"
exit 1
