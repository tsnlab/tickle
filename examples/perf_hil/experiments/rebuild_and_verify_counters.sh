#!/usr/bin/env bash
# Rebuild the perf workspace from HEAD and PROVE the delivery-order counters are in the binary the
# benchmark will actually load, before any arm is run against it.
#
# Why this is a script and not three commands: the arms compare per-run disorder counts, so a build
# without the counters produces zeros in both arms and reads exactly like a fix. That is the day's
# recurring defect, and the only defence is to confirm the instrument exists in the artefact rather
# than to confirm the build exited 0.
#
# Three checks, in order of what they can rule out:
#   1. the counter strings are present in librmw_tickle.so itself
#   2. a real two-node run prints the teardown counters at all
#   3. rx_via_data and rx_via_well_known are BOTH non-zero in that run - if either is zero the run
#      never interleaved the two sockets, so it is void for the question the arms ask rather than a
#      clean zero (d2801a55's own framing, applied to the instrument instead of to a result)
set -eo pipefail

: "${ROS_DISTRO_NAME:?set ROS_DISTRO_NAME}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
RMW_PERF_WS="${RMW_PERF_WS:-$HOME/rmw_perf_ws}"
# Timestamped by default. A fixed filename means a run that never started leaves the PREVIOUS
# run's file in place, and a summary read from it is indistinguishable from a fresh result -
# which happened on 2026-09-24: an interleaved sweep sat blocked on the rig lock while its
# predecessor's numbers were about to be reported as the after-measurement. The tell was that
# they matched to three significant figures.
OUT="${OUT:-/tmp/tickle_rebuild_verify_$(date +%Y%m%d-%H%M%S).txt}"
ln -sfn "$OUT" "/tmp/tickle_rebuild_verify_latest.txt"
export TICKLE_BROADCAST_ADDR="${TICKLE_BROADCAST_ADDR:-127.255.255.255}"

: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== rebuild + counter verification, $(date -Is) ==="
say "repo HEAD: $(git -C "$REPO_ROOT" rev-parse --short HEAD)"

cd "$REPO_ROOT"
# shellcheck disable=SC1090,SC1091
source "/opt/ros/$ROS_DISTRO_NAME/setup.bash"
# shellcheck disable=SC1091
source "$RMW_PERF_WS/install/setup.bash"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
say "--- building rmw_tickle from HEAD ---"
colcon build --packages-select rmw_tickle rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp \
    --cmake-args -DBUILD_SHARED_LIBS=ON >/tmp/rebuild_rmw.log 2>&1 || { tail -20 /tmp/rebuild_rmw.log; exit 1; }
# shellcheck disable=SC1091
source "$REPO_ROOT/install/setup.bash"

say "--- rebuilding buildfarm_perf_tests ---"
colcon build --base-paths "$RMW_PERF_WS" --build-base "$RMW_PERF_WS/build" \
    --install-base "$RMW_PERF_WS/install" --packages-select buildfarm_perf_tests \
    --cmake-args -DPERF_TEST_RUNTIME="${PERF_TEST_RUNTIME:-10}" \
    -DPERF_TEST_TOPICS="Array1k;Struct16" -DPERF_TEST_RMW_IMPLEMENTATIONS="rmw_tickle" \
    >/tmp/rebuild_bpt.log 2>&1 || { tail -20 /tmp/rebuild_bpt.log; exit 1; }
fi

say ""
say "--- check 1: are the counters in the library the benchmark loads? ---"
LIB="$REPO_ROOT/install/rmw_tickle/lib/librmw_tickle.so"
say "library: $LIB ($(date -r "$LIB" +%H:%M:%S))"
# Read the strings ONCE into a variable rather than piping into `grep -q` per symbol. Under
# `set -o pipefail` that pipeline returns 141: grep -q exits on the first match, strings dies of
# SIGPIPE, and pipefail reports the pipeline as failed *because the match was found early*. The
# first version of this check reported all six symbols MISSING against a library that contained all
# six - a check that could not pass, which is the exact mirror of the checks that could not fail
# this project spent two days removing. Confirmed by isolating it: same command, same file, "found"
# without pipefail and "NOT found" with it.
say "  diagnostic: strings=$(command -v strings) lib_exists=$([ -f "$LIB" ] && echo yes || echo no) size=$(stat -c %s "$LIB" 2>/dev/null)"
libstrings="$(strings "$LIB")"
say "  diagnostic: libstrings bytes=${#libstrings}"
[ -n "$libstrings" ] || { say "  strings produced nothing for $LIB - the check itself is broken, not the library"; exit 1; }
missing=0
for sym in rx_via_data rx_via_well_known via_socket_flips out_of_order writer_switches timestamp_not_newer; do
    # Here-string, not a pipe. The bug this replaces was diagnosed correctly and then reproduced
    # verbatim one line later: `strings | grep -q` was replaced by `printf | grep -q`, which has the
    # identical fault. The hazard is not the producer, it is `grep -q` closing the pipe on its first
    # match - under `set -o pipefail` that makes the producer die of SIGPIPE and the pipeline report
    # 141, i.e. FAILURE BECAUSE THE MATCH WAS FOUND. A here-string has no second process to kill.
    if grep -q -- "$sym" <<<"$libstrings"; then say "  present: $sym"; else say "  MISSING: $sym"; missing=1; fi
done
# A positive control, because "all six missing" is also what a broken check looks like: a string
# that is unquestionably in tickle.c must be found, or the check is not reading what it thinks.
grep -q -- "presumed dead" <<<"$libstrings" || { say "  control string absent too - this check is not reading tickle.c's strings at all"; exit 1; }
say "  (control: a known tickle.c string is present, so the check is reading the right binary)"
[ "$missing" -eq 0 ] || { say "the instrument is not in the binary - stopping before any arm is run"; exit 1; }

say ""
say "--- check 2+3: one throwaway matrix run, read the teardown counters ---"
rm -rf "$RMW_PERF_WS/build/buildfarm_perf_tests/test_results"
colcon test --base-paths "$RMW_PERF_WS" --build-base "$RMW_PERF_WS/build" \
    --install-base "$RMW_PERF_WS/install" --packages-select buildfarm_perf_tests \
    --ctest-args -R two_process_rmw_ >/tmp/rebuild_test.log 2>&1 || true
found=0
for log in "$RMW_PERF_WS/build/buildfarm_perf_tests/launch_test/"*two_process_rmw_*.txt; do
    [ -f "$log" ] || continue
    line=$(grep -aoE "rx_via_data=[0-9]+[^\"]*" "$log" | tail -1)
    [ -n "$line" ] || continue
    found=1
    say "  $(basename "$log" .txt): $line"
done
[ "$found" -eq 1 ] || { say "no teardown counter line found in any cell - the counters are in the library but never printed"; exit 1; }
say ""
say "=== read the two numbers above: if rx_via_data or rx_via_well_known is 0 in every cell, the"
say "=== arms cannot discriminate anything and the design needs changing before reps are spent."
