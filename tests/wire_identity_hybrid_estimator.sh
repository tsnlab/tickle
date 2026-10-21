#!/usr/bin/env bash
# tt_InitOptions bundle 7 (ROADMAP "Context options"): at the default options the hybrid transport's estimator and
# control law decide exactly as they did on constants - every announce the reader sends (its ceiling and loss_p), its
# state at every tick, and a RELIABLE writer's n for a failed fragment path.
#
# Builds tests/wire_identity_hybrid_estimator.c (the scenario is described there) against PARENT and against this tree,
# and compares the traces byte for byte:
#
#   control  the parent against itself, two separate builds - identical, or the trace is not deterministic and the
#            comparisons below could not decide anything;
#   size     the parent's trace is large enough to hold the scenario's ticks and announces;
#   change   this tree against the parent - identical;
#   mutant1  this tree with the default n_max the options resolve to one less - must DIFFER (the writer's n, the cap);
#   mutant2  ... the default none_seen_frames 500 more (past the first sample) - must DIFFER (the announced loss_p);
#   mutant3  ... the default collapse_ratio 30 - must DIFFER (the control law: the squeeze no collapse);
#   mutant4  ... the default path_clean_ns 50 s - must DIFFER (the writer's MTU-only time).
#
# Usage: tests/wire_identity_hybrid_estimator.sh [PARENT]     PARENT defaults to ed1d3a2a, the parent of the hybrid
# options. Prints one line per check and exits non-zero if any check fails.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-ed1d3a2a}
CC=${CC:-gcc}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/wire_identity_hybrid_estimator.XXXXXX")
cleanup() {
    git -C "$ROOT" worktree remove --force "$TMP/parent" >/dev/null 2>&1 || true
    rm -rf "$TMP"
}
trap cleanup EXIT

git -C "$ROOT" worktree add --detach "$TMP/parent" "$PARENT" >/dev/null 2>&1

# capture TREE TICKLE_C NAME: the scenario, copied out so that its "test_mock.h" is TREE's, built against TREE's
# headers and the given tickle.c, run; its trace is $TMP/NAME.bin, what it reached $TMP/run_NAME.log.
capture() {
    local tree=$1 tickle_c=$2 name=$3
    mkdir -p "$TMP/src_$name"
    cp "$ROOT/tests/wire_identity_hybrid_estimator.c" "$TMP/src_$name/"
    "$CC" -O0 -g -Wall -Wextra -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tickle_c\"" \
        -DWIRE_CAPTURE_OUT="\"$TMP/$name.bin\"" -o "$TMP/bin_$name" "$TMP/src_$name/wire_identity_hybrid_estimator.c" \
        "$tree/src/encoding.c" "$tree/src/log.c" -lm 2> "$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    "$TMP/bin_$name" > "$TMP/run_$name.log" 2>&1 || {
        tail -5 "$TMP/run_$name.log" >&2
        return 1
    }
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

# mutant TAG OLD NEW: this tree's tickle.c with OLD (which must occur exactly once) replaced by NEW.
mutant() {
    python3 - "$ROOT/src/tickle.c" "$TMP/$1_tickle.c" "$2" "$3" <<'EOF'
import sys
text = open(sys.argv[1]).read()
if text.count(sys.argv[3]) != 1:
    sys.exit(f"mutant anchor matched {text.count(sys.argv[3])} times, not 1")
open(sys.argv[2], "w").write(text.replace(sys.argv[3], sys.argv[4]))
EOF
    capture "$ROOT" "$TMP/$1_tickle.c" "$1"
}

capture "$TMP/parent" "$TMP/parent/src/tickle.c" parent_a
capture "$TMP/parent" "$TMP/parent/src/tickle.c" parent_b
verdict control same "$TMP/parent_a.bin" "$TMP/parent_b.bin"
printf '      reached  %s\n' "$(cat "$TMP/run_parent_a.log")"
# Every phase decided something: a counter left at 0 is a phase the trace says nothing about.
for counter in collapses probes rescales path_failures stale_forgets receiver_lost; do
    if ! grep -Eq "(^| )$counter=[1-9]" "$TMP/run_parent_a.log"; then
        printf 'FAIL  %-8s %s\n' size "the parent's run reached no $counter"
        failed=1
    fi
done
if [ "$(wc -c < "$TMP/parent_a.bin")" -lt 10000 ]; then
    printf 'FAIL  %-8s %s\n' size "the parent's trace is only $(wc -c < "$TMP/parent_a.bin") bytes"
    failed=1
fi

capture "$ROOT" "$ROOT/src/tickle.c" change
verdict change same "$TMP/parent_a.bin" "$TMP/change.bin"

mutant mutant1 "hybrid->n_max = (uint8_t)tt_HYBRID_N_MAX;" "hybrid->n_max = (uint8_t)(tt_HYBRID_N_MAX - 1);"
verdict mutant1 differ "$TMP/parent_a.bin" "$TMP/mutant1.bin"
mutant mutant2 "hybrid->none_seen_frames = (uint32_t)tt_HYBRID_NONE_SEEN_FRAMES;" \
    "hybrid->none_seen_frames = (uint32_t)tt_HYBRID_NONE_SEEN_FRAMES + 500U;"
verdict mutant2 differ "$TMP/parent_a.bin" "$TMP/mutant2.bin"
mutant mutant3 "hybrid_option_or(hybrid->collapse_ratio, tt_HYBRID_COLLAPSE_RATIO);" \
    "hybrid_option_or(hybrid->collapse_ratio, 30.0);"
verdict mutant3 differ "$TMP/parent_a.bin" "$TMP/mutant3.bin"
mutant mutant4 "? (uint64_t)tt_HYBRID_PATH_CLEAN_NS" "? (uint64_t)(50ULL * tt_SECOND)"
verdict mutant4 differ "$TMP/parent_a.bin" "$TMP/mutant4.bin"

cleanup # the trap does it too, on any earlier exit
trap - EXIT
exit "$failed"
