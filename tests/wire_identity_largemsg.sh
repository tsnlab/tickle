#!/usr/bin/env bash
# Large-message stage 2, pre-registration L1.2 (DESIGN.md section 8): small samples are byte-identical on the wire.
#
# Captures the datagrams of 64 B, 1,472 B and 64,000 B samples (tests/test_wire_identity_largemsg.c, the same source
# `make test` runs) and an announce from three builds and compares them, version byte and timestamp masked, and in the
# announce the three tt_AnnounceHeader fields tt_VERSION 13 added (with the padding they move):
#
#   control  the parent against itself, two separate builds - identical, or the capture is not deterministic and
#            the comparison below could not decide anything;
#   golden   the parent's capture is what tests/wire_identity/largemsg_parent.bin holds - identical, or the committed
#            reference is not the parent's (--regenerate rewrites it from the parent);
#   change   this tree against the parent - identical: what `make test` asserts;
#   mutant   this tree with every sample sent as a large one (FRAG_FIRST_L/_CONT_L) - must DIFFER, or the comparison
#            cannot see the change it exists to catch;
#   mutant2  this tree with the announce's entity count off by one - must DIFFER, or the announce mask hides more
#            than the fields it is meant to.
#
# Usage: tests/wire_identity_largemsg.sh [--regenerate] [PARENT]     PARENT defaults to b6d23033, the parent of the
# hybrid transport's tt_VERSION 13 (a47b85e1, stage 2's parent, captured the same small-sample bytes). Prints one line per check and exits non-zero if any check fails.
set -euo pipefail

regenerate=0
if [ "${1:-}" = "--regenerate" ]; then
    regenerate=1
    shift
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-b6d23033}
GOLDEN="$ROOT/tests/wire_identity/largemsg_parent.bin"
CC=${CC:-gcc}

TMP=$(mktemp -d)
cleanup() {
    git -C "$ROOT" worktree remove --force "$TMP/parent" >/dev/null 2>&1 || true
    rm -rf "$TMP"
}
trap cleanup EXIT

git -C "$ROOT" worktree add --detach "$TMP/parent" "$PARENT" >/dev/null 2>&1

# capture TREE SOURCE_TICKLE_C OUT: builds the capture against TREE's headers and the given tickle.c, and runs it.
capture() {
    local tree=$1 tickle_c=$2 out=$3 bin
    bin="$TMP/capture_$(basename "$out" .bin)"
    "$CC" -O0 -g -Wall -Wextra -I"$tree/include" -I"$tree/src" -I"$ROOT/tests" \
        -DTICKLE_C="\"$tickle_c\"" -DWIRE_CAPTURE_OUT="\"$out\"" \
        -o "$bin" "$ROOT/tests/test_wire_identity_largemsg.c" "$tree/src/encoding.c" "$tree/src/log.c" -lm \
        2> "$TMP/build_$(basename "$out" .bin).log" || {
        cat "$TMP/build_$(basename "$out" .bin).log" >&2
        return 1
    }
    "$bin" > /dev/null 2>&1
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

capture "$TMP/parent" "$TMP/parent/src/tickle.c" "$TMP/parent_a.bin"
capture "$TMP/parent" "$TMP/parent/src/tickle.c" "$TMP/parent_b.bin"
verdict control same "$TMP/parent_a.bin" "$TMP/parent_b.bin"

if [ "$regenerate" = 1 ]; then
    mkdir -p "$(dirname "$GOLDEN")"
    cp "$TMP/parent_a.bin" "$GOLDEN"
    echo "regenerated $GOLDEN from $PARENT"
fi
verdict golden same "$TMP/parent_a.bin" "$GOLDEN"

capture "$ROOT" "$ROOT/src/tickle.c" "$TMP/change.bin"
verdict change same "$TMP/parent_a.bin" "$TMP/change.bin"

# The mutant: every sample above 0 bytes takes the large path. The anchor must match exactly once (it had gone stale
# by b6d23033, when the large branch moved ahead of the range check, and the script stopped there).
python3 - "$ROOT/src/tickle.c" "$TMP/mutant_tickle.c" <<'EOF'
import sys
text = open(sys.argv[1]).read()
for old, new in [
    ("#if tt_LARGE_SAMPLES\n    if (cdr_len > tt_MAX_SAMPLE_LENGTH) {\n        return publish_large_instead(",
     "#if tt_LARGE_SAMPLES\n    if (cdr_len > 0) {\n        return publish_large_instead("),
]:
    if text.count(old) != 1:
        sys.exit(f"mutant anchor matched {text.count(old)} times, not 1")
    text = text.replace(old, new)
open(sys.argv[2], "w").write(text)
EOF
capture "$ROOT" "$TMP/mutant_tickle.c" "$TMP/mutant.bin" || true # its headers come from $ROOT/src via -I
if [ -s "$TMP/mutant.bin" ]; then
    verdict mutant differ "$TMP/parent_a.bin" "$TMP/mutant.bin"
else
    echo "FAIL  mutant   produced no capture"
    failed=1
fi

# The second mutant: the announce's entity count off by one, which the announce mask must not hide.
python3 - "$ROOT/src/tickle.c" "$TMP/mutant2_tickle.c" <<'EOF2'
import sys
text = open(sys.argv[1]).read()
old = "    announce->entity_count = (uint8_t)entity_count;\n\n    bool is_flush"
new = "    announce->entity_count = (uint8_t)(entity_count + 1);\n\n    bool is_flush"
if text.count(old) != 1:
    sys.exit(f"mutant2 anchor matched {text.count(old)} times, not 1")
open(sys.argv[2], "w").write(text.replace(old, new))
EOF2
capture "$ROOT" "$TMP/mutant2_tickle.c" "$TMP/mutant2.bin" || true
if [ -s "$TMP/mutant2.bin" ]; then
    verdict mutant2 differ "$TMP/parent_a.bin" "$TMP/mutant2.bin"
else
    echo "FAIL  mutant2  produced no capture"
    failed=1
fi

cleanup # the trap does it too, on any earlier exit
trap - EXIT
exit "$failed"
