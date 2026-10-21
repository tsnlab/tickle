#!/usr/bin/env bash
# tt_InitOptions bundle 1 (ROADMAP "Context options"): a context created with the default options sends what
# tt_Context_create() sent before the options existed - byte for byte, destinations included.
#
# Captures tests/test_wire_identity_initoptions.c's scenario (the same source `make test` runs) and checks:
#
#   control  the parent against itself, two separate builds - identical, or the capture is not deterministic and the
#            comparisons below could not decide anything;
#   golden   the parent's capture is what tests/wire_identity/initoptions_parent.bin holds - identical, or the
#            committed reference is not the parent's (--regenerate rewrites it from the parent);
#   change   this tree's test, which compares tt_Context_create(), tt_Context_create_with(NULL) and
#            tt_Context_create_with(tt_InitOptions_default()) with each other and with the golden - must pass;
#   mutant1  this tree with tt_Context_create()'s reading of _tt_CONFIG.ip_reassembly_max changed - must FAIL, or the
#            legacy arm is not really compared with the parent;
#   mutant2  this tree with the default ip_reassembly_max of the options (not of _tt_CONFIG) changed - must FAIL, or
#            the create_with() arms are not really compared (the legacy arm does not read that default).
#
# Usage: tests/wire_identity_initoptions.sh [--regenerate] [PARENT]     PARENT defaults to 83ef91b6, the parent of the
# options. Prints one line per check and exits non-zero if any check fails.
set -euo pipefail

regenerate=0
if [ "${1:-}" = "--regenerate" ]; then
    regenerate=1
    shift
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-83ef91b6}
GOLDEN="$ROOT/tests/wire_identity/initoptions_parent.bin"
CC=${CC:-gcc}

TMP=$(mktemp -d)
cleanup() {
    git -C "$ROOT" worktree remove --force "$TMP/parent" >/dev/null 2>&1 || true
    rm -rf "$TMP"
}
trap cleanup EXIT

git -C "$ROOT" worktree add --detach "$TMP/parent" "$PARENT" >/dev/null 2>&1

# build TREE TICKLE_C NAME [EXTRA...]: the test source, copied out so that its "test_mock.h" is TREE's (a parent's mock
# implements the parent's HAL), built against TREE's headers and the given tickle.c. Prints the binary's path.
build() {
    local tree=$1 tickle_c=$2 name=$3
    shift 3
    mkdir -p "$TMP/src_$name"
    cp "$ROOT/tests/test_wire_identity_initoptions.c" "$TMP/src_$name/"
    "$CC" -O0 -g -Wall -Wextra -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tickle_c\"" "$@" \
        -o "$TMP/bin_$name" "$TMP/src_$name/test_wire_identity_initoptions.c" "$tree/src/encoding.c" \
        "$tree/src/log.c" -lm 2> "$TMP/build_$name.log" || {
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
        printf 'PASS  %-8s %s\n' "$1" "$(wc -c < "$3") vs $(wc -c < "$4") bytes, $same as expected"
    else
        printf 'FAIL  %-8s %s\n' "$1" "expected $2, got $same"
        failed=1
    fi
}
outcome() { # name expected(pass|fail) binary
    local got=fail
    "$3" > "$TMP/run_$1.log" 2>&1 && got=pass
    if [ "$got" = "$2" ]; then
        printf 'PASS  %-8s %s\n' "$1" "the test ${got}ed, as expected"
    else
        printf 'FAIL  %-8s %s\n' "$1" "expected the test to $2, it ${got}ed"
        sed 's/^/        /' "$TMP/run_$1.log" | tail -5
        failed=1
    fi
}

for arm in a b; do
    bin=$(build "$TMP/parent" "$TMP/parent/src/tickle.c" "parent_$arm" -DWIRE_CAPTURE_OUT="\"$TMP/parent_$arm.bin\"")
    "$bin" > /dev/null
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
mutant mutant1 "_tt_CONFIG.ip_reassembly_max != 0 ? _tt_CONFIG.ip_reassembly_max : tt_IP_REASSEMBLY_NONE;" \
    "_tt_CONFIG.ip_reassembly_max != 0 ? (uint16_t)1000U : tt_IP_REASSEMBLY_NONE;"
mutant mutant2 "? (uint16_t)_tt_IP_REASSEMBLY_MAX : tt_IP_REASSEMBLY_NONE;" "? (uint16_t)1000U : tt_IP_REASSEMBLY_NONE;"

cleanup # the trap does it too, on any earlier exit
trap - EXIT
exit "$failed"
