#!/usr/bin/env bash
# w1_bench.sh - WIRE_PLAN.md 9.1 step 1: core_cost_ab.sh over every pre-registered case, the W1 branch against its
# branch point, paired rounds. Each case gets its own output file; the summary at the end lists, per case, recv
# and send paired differences and the W1 counters. Run it detached (global rule 1): ~20 min.
#
# Usage: w1_bench.sh <base ref> <w1 ref>     Results: $OUT_DIR (default /tmp/w1_bench_<date>), "W1_BENCH_DONE".
set -u
BASE=${1:?usage: w1_bench.sh <base ref> <w1 ref>}
W1=${2:?}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR=${OUT_DIR:-/tmp/w1_bench_$(date +%F)}
ROUNDS=${ROUNDS:-20}
SAMPLES=${SAMPLES:-300000}
mkdir -p "$OUT_DIR"
BIG="-Dtt_MAX_PEER_COUNT=32 -Dtt_MAX_DISCOVERED_ENTITIES=64 -DBENCH_MAX_WRITERS=32"
# name|bench args|compile flags (both arms)
CASES=(
    "w1|-w 1|"
    "w1D|-w 1 -D|"
    "w8|-w 8|"
    "w8D|-w 8 -D|"
    "w32|-w 32|$BIG"
    "w32D|-w 32 -D|$BIG"
    "R|-R|"
    "cR|-c -R|"
    "w32collide|-w 32|$BIG -Dtt_RX_ROUTE_SIZE=16"
)
echo "w1_bench $(date -Is) base=$BASE w1=$W1 rounds=$ROUNDS samples=$SAMPLES" >"$OUT_DIR/summary.txt"
for entry in "${CASES[@]}"; do
    IFS='|' read -r name args cflags <<<"$entry"
    BENCH_ARGS="$args" BENCH_CFLAGS="$cflags" OUT="$OUT_DIR/$name.txt" \
        "$HERE/core_cost_ab.sh" -r "$ROUNDS" -n "$SAMPLES" "$BASE" "$W1" >"$OUT_DIR/$name.log" 2>&1
    status=$?
    {
        echo "== $name ($args${cflags:+; $cflags}) exit=$status"
        grep -vE '^\[' "$OUT_DIR/$name.log" | grep -E 'vs|median|n='
        grep -oE 'short_unrouted=[0-9]+' "$OUT_DIR/$name.txt" | sort | uniq -c | sed 's/^/   W1 /'
        grep -oE 'wire_bytes_per_sample=[0-9.]+' "$OUT_DIR/$name.txt" | sort | uniq -c | sed 's/^/   /'
    } >>"$OUT_DIR/summary.txt"
done
echo "W1_BENCH_DONE" >>"$OUT_DIR/summary.txt"
