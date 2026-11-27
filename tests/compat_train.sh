#!/usr/bin/env bash
# The packet-train wire design (examples/perf_hil/experiments/adaptive_pacing/DESIGN.md, 27.6) puts a new bit in
# tt_HeartbeatHeader.flags and a value in tt_AckNackHeader.reserved, both of which every released build must ignore. This
# builds tests/compat_train_driver.c against each of several trees (default: the v2.1.0 tag and this tree), runs it, and
# checks per tree (a build with the packet train answers a TRAIN HEARTBEAT with the span in reserved; a build without it,
# and every build for any other HEARTBEAT, with 0) that
#   R  a reader answers a HEARTBEAT with bit 2 (and with every unknown high bit) set exactly as without, with and without a
#      gap, and that the control (FINAL set) differs - the driver can tell an answer from none;
#   W  a writer treats an ACKNACK with reserved 0xBEEF / 0xFFFF exactly as with 0, and that a different bitmap differs.
#
# Usage: tests/compat_train.sh [TREE-ISH ...]     e.g. tests/compat_train.sh v2.1.0 a0dd559a
# Prints one line per check and exits non-zero if any fails.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/compat_train.XXXXXX")
# shellcheck disable=SC2329 # invoked by the EXIT trap below
cleanup() {
    rm -rf "$TMP"
}
trap cleanup EXIT

trees=("$@")
[ ${#trees[@]} -eq 0 ] && trees=(v2.1.0 WORKTREE)
failed=0

digest_of() { # RUN-OUTPUT NAME -> the "ok=.. sends=.. bytes=.. digest=.." part of NAME's line
    printf '%s\n' "$1" | awk -v n="$2" '$1 == n { $1 = ""; print }'
}

expect_reserved() { # what got expected
    if [ "$2" = "$3" ]; then
        echo "PASS  $tree: $1"
    else
        echo "FAIL  $tree: $1 (reserved=$2, expected $3)"
        failed=1
    fi
}

for tree in "${trees[@]}"; do
    dir="$TMP/$tree"
    mkdir -p "$dir"
    if [ "$tree" = WORKTREE ]; then
        cp -r "$ROOT/src" "$ROOT/include" "$ROOT/tests" "$dir/"
    else
        git -C "$ROOT" archive "$tree" src include tests | tar -x -C "$dir" || {
            echo "FAIL  $tree: cannot extract"
            failed=1
            continue
        }
    fi
    if ! "$CC" -O0 -g -I"$dir/include" -I"$dir/src" -I"$dir/tests" -DTICKLE_C="\"$dir/src/tickle.c\"" \
        -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -o "$dir/driver" "$ROOT/tests/compat_train_driver.c" "$dir/src/encoding.c" "$dir/src/log.c" -lm 2> "$dir/build.log"; then
        cat "$dir/build.log" >&2
        echo "FAIL  $tree: build"
        failed=1
        continue
    fi
    out=$("$dir/driver") || {
        echo "FAIL  $tree: driver exit $?"
        failed=1
        continue
    }
    same() { # name a b : the two variants must be identical
        if [ "$(digest_of "$out" "$2")" = "$(digest_of "$out" "$3")" ] && [ -n "$(digest_of "$out" "$2")" ]; then
            echo "PASS  $tree: $1 ($2 == $3:$(digest_of "$out" "$2"))"
        else
            echo "FAIL  $tree: $1 ($2:$(digest_of "$out" "$2") vs $3:$(digest_of "$out" "$3"))"
            failed=1
        fi
    }
    differ() { # name a b : the two variants must differ (the control of the check above)
        if [ "$(digest_of "$out" "$2")" != "$(digest_of "$out" "$3")" ]; then
            echo "PASS  $tree: $1 ($2 differs from $3)"
        else
            echo "FAIL  $tree: $1 ($2 == $3: the driver cannot tell them apart)"
            failed=1
        fi
    }
    # A reader's ACKNACK to an ordinary HEARTBEAT carries 0 in reserved in every build.
    if printf '%s\n' "$out" | grep -Eq '^R-plain-res +reserved=0$'; then
        echo "PASS  $tree: an ordinary HEARTBEAT is answered with reserved 0"
    else
        echo "FAIL  $tree: an ordinary HEARTBEAT is not answered with reserved 0"
        failed=1
    fi
    if printf '%s\n' "$out" | grep -q '^R-train-span *n/a'; then
        echo "PASS  $tree: no packet train in this build (the reader answers every HEARTBEAT as above)"
    else
        span=$(printf '%s\n' "$out" | sed -n 's/^R-train-span .*reserved=//p')
        short=$(printf '%s\n' "$out" | sed -n 's/^R-train-short .*reserved=//p')
        ordinary=$(printf '%s\n' "$out" | sed -n 's/^R-train-ordinary .*reserved=//p')
        expect_reserved "a TRAIN HEARTBEAT is answered with the span (119 units of 8 us)" "$span" 119
        expect_reserved "a train that did not arrive whole is answered with 0" "$short" 0
        expect_reserved "an ordinary HEARTBEAT after a train is answered with 0" "$ordinary" 0
    fi
    same "reader ignores HEARTBEAT bit 2" R-ctl R-bit2
    same "reader ignores every unknown HEARTBEAT flag bit" R-ctl R-bit2-hi
    same "reader ignores bit 2 with a gap open" R-gap-ctl R-gap-bit2
    differ "control: FINAL set draws no answer" R-ctl R-final
    same "writer ignores ACKNACK reserved 0xBEEF" W-ctl W-reserved
    same "writer ignores ACKNACK reserved 0xFFFF" W-ctl W-reserved-ff
    same "writer ignores reserved on a pure acknowledgement" W-ack-ctl W-ack-reserved
    differ "control: another bitmap draws other repairs" W-ctl W-other
done
exit "$failed"
