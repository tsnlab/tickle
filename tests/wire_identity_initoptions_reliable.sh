#!/usr/bin/env bash
# tt_InitOptions bundle 3 (ROADMAP "Context options"): with the default reliable options a RELIABLE writer and reader
# send what they sent when the reliability timing was config.h's constants - every datagram both ways, its bytes and
# its instant. tests/wire_identity_initoptions.sh cannot see this group (its scenario never retries: changing any of
# the group's defaults leaves its capture as it was), so this one runs tests/test_wire_identity_reliable.c's lossy
# exchange and checks:
#
#   control  the parent against itself, two separate builds - identical, or the capture is not deterministic and the
#            comparisons below could not decide anything;
#   golden   the parent's capture is what tests/wire_identity/reliable_parent.bin holds - identical, or the committed
#            reference is not the parent's (--regenerate rewrites it from the parent);
#   change   this tree's test, which compares the default and the spelled-out options with each other and with the
#            golden - must pass;
#   mutants  this tree with one of the group's defaults changed (retry_initial, retry, backoff_shift, the idle
#            HEARTBEAT period, max_srtt_multiple at 1, where the ceiling binds on any recovery sample) - each must FAIL, or the scenario does not reach that reader and its
#            identity is not shown.
#
# Usage: tests/wire_identity_initoptions_reliable.sh [--regenerate] [PARENT]     PARENT defaults to 580a5ec8, the parent
# of the group. Prints one line per check and exits non-zero if any check fails.
set -euo pipefail

regenerate=0
if [ "${1:-}" = "--regenerate" ]; then
    regenerate=1
    shift
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-580a5ec8}
GOLDEN="$ROOT/tests/wire_identity/reliable_parent.bin"
CC=${CC:-gcc}
SRC=test_wire_identity_reliable.c

TMP=$(mktemp -d)
cleanup() {
    git -C "$ROOT" worktree remove --force "$TMP/parent" >/dev/null 2>&1 || true
    rm -rf "$TMP"
}
trap cleanup EXIT

git -C "$ROOT" worktree add --detach "$TMP/parent" "$PARENT" >/dev/null 2>&1

# build TREE TICKLE_C NAME [EXTRA...]: the test source, copied out so that its "test_mock.h" is TREE's, built against
# TREE's headers and the given tickle.c. Prints the binary's path.
build() {
    local tree=$1 tickle_c=$2 name=$3
    shift 3
    mkdir -p "$TMP/src_$name"
    cp "$ROOT/tests/$SRC" "$TMP/src_$name/"
    "$CC" -O0 -g -Wall -Wextra -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tickle_c\"" "$@" \
        -o "$TMP/bin_$name" "$TMP/src_$name/$SRC" "$tree/src/encoding.c" "$tree/src/log.c" -lm \
        2> "$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    echo "$TMP/bin_$name"
}

failed=0
verdict() { # name expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ]; then
        printf 'PASS  %-24s %s\n' "$1" "$(wc -c < "$3") vs $(wc -c < "$4") bytes, $same as expected"
    else
        printf 'FAIL  %-24s %s\n' "$1" "expected $2, got $same"
        failed=1
    fi
}
outcome() { # name expected(pass|fail) binary
    local got=fail
    "$3" > "$TMP/run_$1.log" 2>&1 && got=pass
    if [ "$got" = "$2" ]; then
        printf 'PASS  %-24s %s\n' "$1" "the test ${got}ed, as expected"
    else
        printf 'FAIL  %-24s %s\n' "$1" "expected the test to $2, it ${got}ed"
        sed 's/^/        /' "$TMP/run_$1.log" | tail -5
        failed=1
    fi
}

for arm in a b; do
    bin=$(build "$TMP/parent" "$TMP/parent/src/tickle.c" "parent_$arm" -DWIRE_CAPTURE_OUT="\"$TMP/parent_$arm.bin\"")
    "$bin" > "$TMP/capture_$arm.log" 2>&1 || {
        echo "FAIL  the parent's capture run failed:"
        tail -5 "$TMP/capture_$arm.log"
        exit 1
    }
done
verdict control same "$TMP/parent_a.bin" "$TMP/parent_b.bin"

if [ "$regenerate" = 1 ]; then
    mkdir -p "$(dirname "$GOLDEN")"
    cp "$TMP/parent_a.bin" "$GOLDEN"
    echo "regenerated $GOLDEN from $PARENT"
fi
verdict golden same "$TMP/parent_a.bin" "$GOLDEN"

outcome change pass "$(build "$ROOT" "$ROOT/src/tickle.c" change -DWIRE_GOLDEN="\"$GOLDEN\"")"

# mutant TAG OLD NEW: this tree's tickle.c with OLD (which must occur exactly once) replaced by NEW.
mutant() {
    python3 - "$ROOT/src/tickle.c" "$TMP/$1_tickle.c" "$2" "$3" <<'EOF'
import sys
text = open(sys.argv[1]).read()
if text.count(sys.argv[3]) != 1:
    sys.exit(f"mutant anchor matched {text.count(sys.argv[3])} times, not 1")
open(sys.argv[2], "w").write(text.replace(sys.argv[3], sys.argv[4]))
EOF
    outcome "$1" fail "$(build "$ROOT" "$TMP/$1_tickle.c" "$1" -DWIRE_GOLDEN="\"$GOLDEN\"")"
}
mutant retry_initial ".retry_initial = (uint64_t)tt_RELIABLE_RETRY_INITIAL," ".retry_initial = 2 * tt_MILLISECOND,"
mutant retry ".retry = (uint8_t)tt_RELIABLE_RETRY," ".retry = 2,"
mutant backoff_shift "? (uint8_t)tt_RELIABLE_RETRY_BACKOFF_SHIFT" "? (uint8_t)2U"
mutant idle_heartbeat_period "tt_IDLE_HEARTBEAT_PERIOD != 0 ? (uint64_t)tt_IDLE_HEARTBEAT_PERIOD" \
    "tt_IDLE_HEARTBEAT_PERIOD != 0 ? (uint64_t)(100 * tt_MILLISECOND)"
mutant max_srtt_multiple ".max_srtt_multiple = (uint16_t)tt_RELIABLE_RETRY_MAX_SRTT_MULTIPLE," ".max_srtt_multiple = 1,"

cleanup # the trap does it too, on any earlier exit
trap - EXIT
exit "$failed"
