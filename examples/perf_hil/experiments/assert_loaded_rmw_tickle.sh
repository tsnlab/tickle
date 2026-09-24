#!/usr/bin/env bash
# Assert which librmw_tickle.so a running benchmark actually has mapped, and that it contains an
# expected marker string. Sourced or called by any harness that reports a number about rmw_tickle.
#
# Why this exists (2026-09-24): $RMW_PERF_WS/install/setup.bash carries baked-in prefixes from
# whoever built that workspace - on this box, the CI runner's own checkout - and they land AHEAD of
# anything sourced afterwards. Sourcing your own install last does not win. Four runs of one
# experiment measured a library from the previous day while reporting on code written that morning,
# and the only clue was a log line whose wording was subtly the old one.
#
# A 144-cell result of mine came out of the same workspace and survived only because the CI build
# that happened to be installed contained the fix - established afterwards from commit ancestry and
# job timestamps rather than from observation. Sourcing order is an environment fact nobody here
# controls. What a process has mapped is observable, so observe it.
#
#   assert_loaded_rmw_tickle <pid> <marker>   - the pid must have it mapped and it must contain <marker>
#   find_perf_test_pid                        - a running perf_test, or empty
set -eo pipefail

find_perf_test_pid() {
    for pid in $(ps -eo pid,cmd --no-headers | awk '/perf_test/ && !/awk/ {print $1}'); do
        [ -r "/proc/$pid/maps" ] || continue
        grep -q 'librmw_tickle\.so' "/proc/$pid/maps" 2>/dev/null && { echo "$pid"; return 0; }
    done
    return 1
}

assert_loaded_rmw_tickle() {
    local pid="$1" marker="$2" lib
    lib=$(grep -o '/[^ ]*librmw_tickle[^ ]*' "/proc/$pid/maps" 2>/dev/null | sort -u | head -1)
    if [ -z "$lib" ]; then
        echo "assert_loaded_rmw_tickle: pid $pid has no librmw_tickle.so mapped - VOID, not a pass" >&2
        return 1
    fi
    echo "assert_loaded_rmw_tickle: pid $pid mapped $lib ($(date -r "$lib" +%Y-%m-%d\ %H:%M:%S))"
    # Here-string, not a pipe into grep -q: under `set -o pipefail` grep -q exits on its first match,
    # the producer dies of SIGPIPE and the pipeline reports 141 - failure BECAUSE the match was
    # found. That cost an afternoon on 2026-09-24 and was reproduced once while being fixed.
    local libstrings
    libstrings="$(strings "$lib")"
    if ! grep -q -- "$marker" <<<"$libstrings"; then
        echo "assert_loaded_rmw_tickle: $lib does NOT contain '$marker' - it is not the build under test" >&2
        return 1
    fi
    echo "assert_loaded_rmw_tickle: marker '$marker' present - this is the build under test"
}

# Run directly for a one-shot check against whatever is running now.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    marker="${1:?usage: assert_loaded_rmw_tickle.sh <marker-string>}"
    pid=$(find_perf_test_pid) || { echo "no running perf_test has librmw_tickle.so mapped" >&2; exit 1; }
    assert_loaded_rmw_tickle "$pid" "$marker"
fi
