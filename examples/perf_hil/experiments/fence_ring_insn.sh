#!/usr/bin/env bash
# A/B of the segment ring's instructions per record (fence_ring_insn.c), base tickle.c vs another tree, interleaved
# A B A B ... ROUNDS times on this PC. No sockets: the bench runs on the mock HAL, so no netns is needed.
#
# Reading rules, enforced below, decided before the run (2026-10-11):
#   VOID   the control (segment_has_records(), untouched by the change) differs between A and B by more than 0.5%,
#          or any round's count is missing.
#   PASS   B's median is within 1 instruction per record of A's for both write and drain: the fence adds nothing to
#          a record's path, up to code generation moving a compare.
#   FAIL   otherwise (B dearer by more than 1 instruction per record on either path).
#
# Usage: fence_ring_insn.sh [BASE] [B] [ROUNDS]   (BASE default origin/main; B a commit, default the working tree;
#   ROUNDS 10). Output: per-round lines, a median table, and a final "VERDICT=<PASS|FAIL|VOID>".
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${1:-origin/main}
BREF=${2:-}
ROUNDS=${3:-10}
CC=${CC:-cc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fence_insn.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/A" "$WORK/B"
git -C "$ROOT" archive "$BASE" src include tests/test_mock.h | tar -x -C "$WORK/A"
if [ -n "$BREF" ]; then
    git -C "$ROOT" archive "$BREF" src include tests/test_mock.h | tar -x -C "$WORK/B"
    bname="$BREF ($(git -C "$ROOT" rev-parse --short "$BREF"))"
else
    cp -r "$ROOT/src" "$ROOT/include" "$WORK/B/"
    mkdir -p "$WORK/B/tests" && cp "$ROOT/tests/test_mock.h" "$WORK/B/tests/"
    bname="working tree on $(git -C "$ROOT" rev-parse --short HEAD)"
fi
for arm in A B; do
    mkdir -p "$WORK/$arm/examples/perf_hil/experiments"
    cp "$ROOT/examples/perf_hil/experiments/fence_ring_insn.c" "$WORK/$arm/examples/perf_hil/experiments/"
    "$CC" -O2 -DNDEBUG -w -I"$WORK/$arm/include" -I"$WORK/$arm/src" -o "$WORK/$arm.bin" \
        "$WORK/$arm/examples/perf_hil/experiments/fence_ring_insn.c" "$WORK/$arm/src/encoding.c" \
        "$WORK/$arm/src/log.c" -lm
done
echo "A=$BASE ($(git -C "$ROOT" rev-parse --short "$BASE"))  B=$bname"

: >"$WORK/all"
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
    arm = $1; c = $3; x = $4; sub(/^insn_per_record=/, "", x)
    key = arm SUBSEP c
    vals[key] = vals[key] " " x; count[key]++
    cases[c] = 1
}
END {
    verdict = "PASS"
    printf "\n%-8s %12s %12s %10s\n", "case", "A median", "B median", "B-A"
    for (c in cases) {
        ka = "A" SUBSEP c; kb = "B" SUBSEP c
        if (count[ka] != rounds || count[kb] != rounds) { print "missing rounds for " c; verdict = "VOID"; continue }
        a = median(ka); b = median(kb); med[ka] = a; med[kb] = b
        printf "%-8s %12.3f %12.3f %10.3f\n", c, a, b, b - a
    }
    a = med["A" SUBSEP "control"]; b = med["B" SUBSEP "control"]
    if (a == 0 || (b - a) / a > 0.005 || (a - b) / a > 0.005) { print "control moved"; verdict = "VOID" }
    if (verdict != "VOID") {
        if (med["B" SUBSEP "write"] - med["A" SUBSEP "write"] > 1) verdict = "FAIL"
        if (med["B" SUBSEP "drain"] - med["A" SUBSEP "drain"] > 1) verdict = "FAIL"
    }
    print "VERDICT=" verdict
}' "$WORK/all"
