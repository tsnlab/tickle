#!/usr/bin/env bash
# The discovery summary's timing bytes (the user's decision of 2026-10-22, network-profiles item C): a default context
# sends zero in tt_HeartbeatHeader's three former reserved bytes, so its datagrams are the parent's byte for byte,
# destinations included.
#
# Captures tests/test_wire_identity_initoptions.c's scenario (creation, publishers, a subscriber, samples, then 3 s of
# polling: announces, three summaries, heartbeats) from a tree's tt_Context_create() and compares:
#
#   control  the parent against itself, two separate builds - identical, or the capture is not deterministic and the
#            comparisons below could not decide anything;
#   change   this tree against the parent - identical;
#   mutant   this tree with the default miss threshold sent as itself (3, not 0) - must DIFFER, or the capture does not
#            reach the summary's bytes and "identical" above says nothing about them.
#
# Usage: tests/wire_identity_discovery_timing.sh [PARENT]     PARENT defaults to 6c355fa2, the parent of the change.
# Prints one line per check and exits non-zero if any check fails.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-6c355fa2}
CC=${CC:-gcc}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/wire_identity_discovery_timing.XXXXXX")
# shellcheck disable=SC2329 # invoked by the EXIT trap below
cleanup() { # the one EXIT trap
    rm -rf "$TMP"
}
trap cleanup EXIT

mkdir -p "$TMP/parent"
git -C "$ROOT" archive "$PARENT" src include tests | tar -x -C "$TMP/parent"

# capture TREE TICKLE_C NAME: the scenario built against TREE's headers and mock HAL and the given tickle.c, run;
# its capture lands in $TMP/NAME.bin.
capture() {
    local tree=$1 tickle_c=$2 name=$3
    mkdir -p "$TMP/src_$name"
    cp "$ROOT/tests/test_wire_identity_initoptions.c" "$TMP/src_$name/"
    "$CC" -O0 -g -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tickle_c\"" \
        -DWIRE_CAPTURE_OUT="\"$TMP/$name.bin\"" -o "$TMP/bin_$name" "$TMP/src_$name/test_wire_identity_initoptions.c" \
        "$tree/src/encoding.c" "$tree/src/log.c" -lm 2> "$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    "$TMP/bin_$name" > "$TMP/run_$name.log" 2>&1
}

failed=0
verdict() { # name expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ] && [ -s "$3" ]; then
        printf 'PASS  %-8s %s\n' "$1" "$(wc -c < "$3") vs $(wc -c < "$4") bytes, $same as expected"
    else
        printf 'FAIL  %-8s %s\n' "$1" "expected $2 (and a capture), got $same, $(wc -c < "$3") bytes"
        failed=1
    fi
}

capture "$TMP/parent" "$TMP/parent/src/tickle.c" parent_a
capture "$TMP/parent" "$TMP/parent/src/tickle.c" parent_b
verdict control same "$TMP/parent_a.bin" "$TMP/parent_b.bin"

capture "$ROOT" "$ROOT/src/tickle.c" change
verdict change same "$TMP/change.bin" "$TMP/parent_a.bin"

python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" << 'EOF'
import sys
text = open(sys.argv[1]).read()
old = "    summary->summary_miss_threshold = miss == (uint64_t)tt_LIVELINESS_MISS_THRESHOLD ? 0 : (uint8_t)miss;"
if text.count(old) != 1:
    sys.exit(f"mutant anchor matched {text.count(old)} times, not 1")
open(sys.argv[2], "w").write(text.replace(old, "    summary->summary_miss_threshold = (uint8_t)miss;"))
EOF
capture "$ROOT" "$TMP/mutant_tickle.c" mutant
verdict mutant differ "$TMP/mutant.bin" "$TMP/parent_a.bin"

exit "$failed"
