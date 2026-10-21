#!/usr/bin/env bash
# A/B of instructions per service call (kept_answer_insn.c), the base tickle.c vs the change (2026-10-21,
# ab/kept-answer-lazy-expiry: a kept answer's expiry as a time in its slot, not a scheduler entry), interleaved
# A B A B ... ROUNDS times on this PC. No sockets: the bench runs on the mock HAL, so no netns is needed.
#
# Reading rules, enforced below, decided before the run (2026-10-21):
#   VOID   if any case printed VOID (a call not run exactly once or not answered), a round's count is missing, or the
#          control (the Client's processing of the answer, which the change does not touch) differs between A and B
#          by more than 0.5% in any case - the builds would then differ in something besides the change.
#   PASS   if B's server count is not above A's in any case by more than the floor, max(1 instruction, A's own
#          round-to-round spread): the per-call cost goes down or stays.
#   FAIL   otherwise. total is reported, not judged (it includes the control).
#
# Usage: examples/perf_hil/experiments/kept_answer_insn.sh [A] [B] [ROUNDS]
#   A default origin/main; B a commit, default the working tree; ROUNDS 7.
# Output: per-round lines, a median table, and a final "VERDICT=<PASS|FAIL|VOID>".
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
A_REF=${1:-origin/main}
B_REF=${2:-}
ROUNDS=${3:-7}
CC=${CC:-cc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kept_answer_insn.XXXXXX")
trap 'rm -rf "$WORK"' EXIT # the only EXIT trap here

mkdir -p "$WORK/A" "$WORK/B"
git -C "$ROOT" archive "$A_REF" src include tests/test_mock.h | tar -x -C "$WORK/A"
if [ -n "$B_REF" ]; then
    git -C "$ROOT" archive "$B_REF" src include tests/test_mock.h | tar -x -C "$WORK/B"
    B_NAME=$(git -C "$ROOT" rev-parse --short "$B_REF")
else
    cp -r "$ROOT/src" "$ROOT/include" "$WORK/B/"
    mkdir -p "$WORK/B/tests" && cp "$ROOT/tests/test_mock.h" "$WORK/B/tests/"
    B_NAME="working tree on $(git -C "$ROOT" rev-parse --short HEAD)"
fi
for arm in A B; do # the bench is this checkout's, placed in each arm's tree: it includes ../../../src/tickle.c
    mkdir -p "$WORK/$arm/examples/perf_hil/experiments"
    cp "$ROOT/examples/perf_hil/experiments/kept_answer_insn.c" "$WORK/$arm/examples/perf_hil/experiments/"
    "$CC" -O2 -DNDEBUG -w -I"$WORK/$arm/include" -I"$WORK/$arm/src" -o "$WORK/$arm.bin" \
        "$WORK/$arm/examples/perf_hil/experiments/kept_answer_insn.c" "$WORK/$arm/src/encoding.c" \
        "$WORK/$arm/src/log.c" -lm -lpthread
done
echo "A=$A_REF ($(git -C "$ROOT" rev-parse --short "$A_REF"))  B=$B_NAME  rounds=$ROUNDS"

: >"$WORK/all"
for round in $(seq 1 "$ROUNDS"); do
    for arm in A B; do
        "$WORK/$arm.bin" | sed "s/^/$arm $round /" | tee -a "$WORK/all"
    done
done

awk -v rounds="$ROUNDS" '
function median(key,   n, i, j, t, v) {
    n = split(vals[key], v, " ")
    for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (v[j] + 0 < v[i] + 0) { t = v[i]; v[i] = v[j]; v[j] = t }
    return n % 2 ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
}
function spread(key,   n, i, v, lo, hi) {
    n = split(vals[key], v, " "); lo = v[1]; hi = v[1]
    for (i = 2; i <= n; i++) { if (v[i] + 0 < lo + 0) lo = v[i]; if (v[i] + 0 > hi + 0) hi = v[i] }
    return hi - lo
}
$4 == "VOID" { print "VOID run: " $0; void = 1; next }
{
    arm = $1; c = $3
    for (f = 4; f <= NF; f++) {
        split($f, kv, "="); m = kv[1]
        if (m == "server" || m == "control" || m == "total") {
            key = arm SUBSEP c SUBSEP m; vals[key] = vals[key] " " kv[2]; count[key]++
        }
        if (m == "calls") calls[c] = kv[2]
    }
    cases[c] = 1
}
END {
    verdict = void ? "VOID" : "PASS"
    printf "\n%-9s %-8s %7s %12s %12s %10s %8s %10s\n", "case", "metric", "calls", "A median", "B median", "B-A", "B-A %", "A spread"
    for (c in cases) for (mi = 1; mi <= 3; mi++) {
        m = mi == 1 ? "server" : mi == 2 ? "control" : "total"
        ka = "A" SUBSEP c SUBSEP m; kb = "B" SUBSEP c SUBSEP m
        if (count[ka] != rounds || count[kb] != rounds) { print "missing rounds for " c " " m; verdict = "VOID"; continue }
        a = median(ka); b = median(kb); s = spread(ka)
        printf "%-9s %-8s %7s %12.2f %12.2f %10.2f %7.2f%% %10.2f\n", c, m, calls[c], a, b, b - a, 100 * (b - a) / a, s
        if (m == "control" && (a == 0 || (b - a) / a > 0.005 || (a - b) / a > 0.005)) { print "control moved in " c; verdict = "VOID" }
        if (m == "server") { floor = s > 1 ? s : 1; if (verdict == "PASS" && b - a > floor) { print "server cost up in " c; fail = 1 } }
    }
    if (verdict == "PASS" && fail) verdict = "FAIL"
    print "VERDICT=" verdict
}' "$WORK/all"
