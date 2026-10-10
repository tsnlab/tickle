#!/usr/bin/env bash
# A/B of update_reliable_ack()'s instructions per call (ack_bitmap_insn.c), base tickle.c vs the working tree,
# interleaved A B A B ... ROUNDS times on this PC. No sockets: the bench runs on the mock HAL, so no netns needed.
#
# Reading rules, enforced below, decided before the run (2026-10-10):
#   VOID   if the control (find_writer_proxy(), untouched by the change) differs between A and B by more than
#          0.5% at any width - the builds would then differ in something besides the change - or if any round's
#          A or B count is missing.
#   PASS   if B's in_order count is below A's at every width, and B's in_order is independent of the window:
#          w=64 minus w=4 within 2 instructions per call (A's grows with the window, by construction).
#   FAIL   otherwise. gap_open is reported, not judged: it shows what the recovery case costs on each side.
#
# Usage: examples/perf_hil/experiments/ack_bitmap_insn.sh [BASE] [ROUNDS]   (BASE default origin/main, ROUNDS 10)
# Output: per-round lines, a median table, and a final "VERDICT=<PASS|FAIL|VOID>".
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${1:-origin/main}
ROUNDS=${2:-10}
CC=${CC:-cc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ack_insn.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/A" "$WORK/B"
git -C "$ROOT" archive "$BASE" src include tests/test_mock.h | tar -x -C "$WORK/A"
cp -r "$ROOT/src" "$ROOT/include" "$WORK/B/"
mkdir -p "$WORK/B/tests" && cp "$ROOT/tests/test_mock.h" "$WORK/B/tests/"
for arm in A B; do
    mkdir -p "$WORK/$arm/examples/perf_hil/experiments"
    cp "$ROOT/examples/perf_hil/experiments/ack_bitmap_insn.c" "$WORK/$arm/examples/perf_hil/experiments/"
    "$CC" -O2 -DNDEBUG -w -I"$WORK/$arm/include" -I"$WORK/$arm/src" -o "$WORK/$arm.bin" \
        "$WORK/$arm/examples/perf_hil/experiments/ack_bitmap_insn.c" "$WORK/$arm/src/encoding.c" \
        "$WORK/$arm/src/log.c" -lm
done
echo "A=$BASE ($(git -C "$ROOT" rev-parse --short "$BASE"))  B=working tree on $(git -C "$ROOT" rev-parse --short HEAD)"

: > "$WORK/all"
for round in $(seq 1 "$ROUNDS"); do
    for arm in A B; do
        "$WORK/$arm.bin" | sed "s/^/$arm $round /" | tee -a "$WORK/all"
    done
done

awk -v rounds="$ROUNDS" '
function median(key,   n, i, j, t, v) {
    n = split(vals[key], v, " ")
    for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (v[j] < v[i]) { t = v[i]; v[i] = v[j]; v[j] = t }
    return n % 2 ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
}
{
    arm = $1; c = $3; w = $4; sub(/^w=/, "", w); x = $5; sub(/^insn_per_call=/, "", x)
    key = arm SUBSEP c SUBSEP w
    vals[key] = vals[key] " " x; count[key]++
    cases[c] = 1; widths[w] = 1
}
END {
    verdict = "PASS"
    printf "\n%-9s %4s %12s %12s %10s\n", "case", "w", "A median", "B median", "B-A"
    for (c in cases) for (w in widths) {
        ka = "A" SUBSEP c SUBSEP w; kb = "B" SUBSEP c SUBSEP w
        if (count[ka] != rounds || count[kb] != rounds) { print "missing rounds for " c " w=" w; verdict = "VOID"; continue }
        a = median(ka); b = median(kb); med[ka] = a; med[kb] = b
        printf "%-9s %4s %12.2f %12.2f %10.2f\n", c, w, a, b, b - a
    }
    for (w in widths) {
        a = med["A" SUBSEP "control" SUBSEP w]; b = med["B" SUBSEP "control" SUBSEP w]
        if (a == 0 || (b - a) / a > 0.005 || (a - b) / a > 0.005) { print "control moved at w=" w; verdict = "VOID" }
    }
    if (verdict != "VOID") {
        for (w in widths) if (med["B" SUBSEP "in_order" SUBSEP w] >= med["A" SUBSEP "in_order" SUBSEP w]) verdict = "FAIL"
        spread = med["B" SUBSEP "in_order" SUBSEP 64] - med["B" SUBSEP "in_order" SUBSEP 4]
        if (spread > 2 || spread < -2) { print "B in_order depends on the window: w64 - w4 = " spread; verdict = "FAIL" }
    }
    print "VERDICT=" verdict
}' "$WORK/all"
