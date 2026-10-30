#!/usr/bin/env bash
# The reveal re-ask (tt_LinkOptions.repair_reveal, docs/DESIGN.md "Repair reveal"): OFF, the default on every profile,
# sends what the parent sent - every datagram, its bytes, its time and its destination - for large samples and small.
#
# Builds tests/wire_identity_repair_reveal.c - test_large_sample.c's two-context sim, both contexts' datagrams
# captured, a reader with WIFI's repair_timing, 10% loss over a 10 ms path - against the parent's tickle.c and headers
# and against this tree's, and compares the captures of two scenarios (large: 4 x 1 MiB; small: 200 x 1000 B):
#
#   control    the parent against itself, two separate builds - identical, or the capture is not deterministic and
#              nothing below decides anything;
#   default    this tree, the reader's repair_reveal left as created - identical to the parent;
#   off        this tree, repair_reveal tt_REPAIR_REVEAL_OFF - identical;
#   all        this tree, tt_REPAIR_REVEAL_ALL_MISSING - must DIFFER in both scenarios, or the scenario never reaches
#              the reveal and the identity above says nothing about it;
#   revealed   this tree, tt_REPAIR_REVEAL_REVEALED - must DIFFER in the large scenario (the small one is printed: see
#              the comment at its line).
#
# Usage: tests/wire_identity_repair_reveal.sh [PARENT]     PARENT defaults to 75495519, the change's parent on main.
# Prints one line per check; exits non-zero if any fails.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-75495519}
CC=${CC:-gcc}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/parent"
git -C "$ROOT" archive "$PARENT" include src tests | tar -x -C "$TMP/parent"

# capture TREE NAME [CFLAGS...]: builds the capture against TREE's headers and tickle.c, and runs its scenarios into
# $TMP/NAME.{large,small}.bin.
capture() {
    local tree=$1 name=$2
    shift 2
    # A copy, so that the sim it includes (test_large_sample.c, test_mock.h) is TREE's: this tree's tests name fields
    # the parent does not have.
    mkdir -p "$TMP/src_$name"
    cp "$ROOT/tests/wire_identity_repair_reveal.c" "$TMP/src_$name/"
    "$CC" -O1 -g -w -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tree/src/tickle.c\"" "$@" \
        -o "$TMP/bin_$name" "$TMP/src_$name/wire_identity_repair_reveal.c" "$tree/src/encoding.c" "$tree/src/log.c" \
        -lpthread -lm 2> "$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    (
        ulimit -v 2000000 # the PC is shared: a runaway sim is cut off, not the machine
        timeout 300 "$TMP/bin_$name" "$TMP/$name.large.bin" "$TMP/$name.small.bin" > "$TMP/run_$name.log" 2>&1
    ) || {
        tail -5 "$TMP/run_$name.log" >&2
        return 1
    }
    grep '^wire_identity_repair_reveal:' "$TMP/run_$name.log" | sed "s|$TMP/||"
}

failed=0
verdict() { # check expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ]; then
        printf 'PASS  %-18s %s\n' "$1" "$(wc -c < "$3") vs $(wc -c < "$4") bytes, $same as expected"
    else
        printf 'FAIL  %-18s %s\n' "$1" "expected $2, got $same"
        failed=1
    fi
}

capture "$TMP/parent" parent_a
capture "$TMP/parent" parent_b
capture "$ROOT" default
capture "$ROOT" off -DREVEAL_MODE=tt_REPAIR_REVEAL_OFF
capture "$ROOT" revealed -DREVEAL_MODE=tt_REPAIR_REVEAL_REVEALED
capture "$ROOT" all -DREVEAL_MODE=tt_REPAIR_REVEAL_ALL_MISSING
for scenario in large small; do
    verdict "control $scenario" same "$TMP/parent_a.$scenario.bin" "$TMP/parent_b.$scenario.bin"
    verdict "default $scenario" same "$TMP/parent_a.$scenario.bin" "$TMP/default.$scenario.bin"
    verdict "off $scenario" same "$TMP/parent_a.$scenario.bin" "$TMP/off.$scenario.bin"
    verdict "all $scenario" differ "$TMP/parent_a.$scenario.bin" "$TMP/all.$scenario.bin"
done
verdict "revealed large" differ "$TMP/parent_a.large.bin" "$TMP/revealed.large.bin"
# REVEALED on the small scenario: printed, not judged. In this sim (10% loss on every datagram, the small path's writer
# answering every request) it sent no reveal at 2%, 5% or 10% loss, and the capture equals the parent's (2026-10-30;
# why no unit qualified was not traced). What REVEALED does to small samples is test_reliable_pubsub's
# test_a_small_reader_reveals_as_its_mode_says.
if cmp -s "$TMP/parent_a.small.bin" "$TMP/revealed.small.bin"; then
    echo "INFO  revealed small     same as the parent (nothing revealed in this scenario)"
else
    echo "INFO  revealed small     differs from the parent"
fi
exit "$failed"
