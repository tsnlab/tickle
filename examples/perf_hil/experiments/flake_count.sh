#!/usr/bin/env bash
# flake_count.sh - runs one test binary RUNS times and counts the runs that did not print its pass line (2026-09-27,
# CONTEXT_NODE_PLAN.md "Client retry fix"). A run that hangs is killed after TIMEOUT seconds and counted as a failure
# too - a hang is how test_thread_safety's retry defect shows since 579dfcaf.
#
# Usage: flake_count.sh BINARY RUNS TIMEOUT OUT [ENV=VALUE...]
# OUT gets one line per failed run (its exit status and the test's own failure lines) and a last line
# "FLAKES n/RUNS", which is also written when the loop ends early, so a reader never takes a partial file for a
# finished one: no FLAKES line means the job did not finish.
set -uo pipefail
BINARY="${1:?binary}"
RUNS="${2:?runs}"
TIMEOUT="${3:?timeout seconds}"
OUT="${4:?output file}"
shift 4
[ -x "$BINARY" ] || { echo "flake_count: $BINARY is not an executable" >&2; exit 1; }
: >"$OUT"
fails=0
done_runs=0
trap 'echo "FLAKES $fails/$done_runs (of $RUNS planned)" >>"$OUT"' EXIT
for run in $(seq "$RUNS"); do
    out="$(cd /tmp && env "$@" timeout "$TIMEOUT" "$BINARY" 2>&1)"
    rc=$?
    done_runs=$run
    if ! grep -q 'all tests passed' <<<"$out"; then
        fails=$((fails + 1))
        echo "run $run rc=$rc $(grep -aE 'expected|ThreadSanitizer|asked again' <<<"$out" | head -3 | tr '\n' ' ')" >>"$OUT"
    fi
done
