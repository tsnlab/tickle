#!/usr/bin/env bash
# Does "Data consistency violated" happen to rmw implementations that contain none of our code?
#
# Why (2026-09-24): two days were spent treating that abort as a TickLE defect. The archived ctest
# logs from 2026-09-23 21:01 and 21:05 - runs that tested three rmw implementations, not one -
# say otherwise:
#
#     LastTest_20260923-1159   cyclonedds 2   fastrtps 0   tickle 0
#     LastTest_20260923-1201   cyclonedds 2   fastrtps 4   tickle 4
#     LastTest_20260923-1203   cyclonedds 2   fastrtps 4   tickle 3 + 1
#
# Four cells per implementation per run. rmw_fastrtps_cpp aborted in all four of its cells, the
# same as TickLE; rmw_cyclonedds_cpp aborted in all three runs including the one where TickLE
# aborted zero times. Nobody noticed because both of us were grepping for "_tickle_", and because
# CI stopped building the other vendors when PERF_TEST_RMW_IMPLEMENTATIONS went rmw_tickle-only.
#
# HOW TO READ THE RESULT - written down before running it, so the answer cannot be chosen after
# seeing the numbers:
#
#   * Any abort under rmw_fastrtps_cpp or rmw_cyclonedds_cpp  -> the abort is not TickLE-specific.
#     It is a property of the benchmark or this machine, and the socket-interleaving arms are an
#     explanation for someone else's behaviour. Stop building them.
#   * Aborts under rmw_tickle only, across all reps          -> a genuine TickLE-specific signal,
#     and the arms are worth the reps.
#   * No aborts anywhere                                     -> inconclusive, not a pass. At the
#     measured ~8% per-cell rate, 12 cells per vendor can easily produce zero by chance. Says
#     nothing either way and must not be reported as "fixed".
#
# The two message variants are counted separately and must not be merged: "not strictly higher id"
# has been seen under all three implementations, while "not strictly older timestamp" has so far
# only been seen under TickLE. The second is the one this investigation was actually about, and
# the archived runs give far fewer non-TickLE cells than TickLE ones, so its absence elsewhere is
# not yet evidence of anything.
#
# CONTROL: the run is void unless every vendor actually executed its four cells. A vendor whose
# tests were never generated reports zero aborts, which is indistinguishable from a vendor that
# ran cleanly - the same "zero tests ran means all passed" failure this repo has already had once.
# No `set -u`: ROS's own setup.bash reads unbound variables and dies under it, taking the whole
# run with it in its first second. This is the second time today that combination has silently
# killed a detached job - the first was caught only because the log was read; a job that dies
# instantly and a job that is still starting up look identical from outside.
set -o pipefail

REPS="${REPS:-3}"
WS="$HOME/rmw_perf_ws"
OUT="${OUT:-/tmp/cross_vendor_abort}"   # deliberately not under any session's scratchpad
VENDORS="rmw_tickle;rmw_fastrtps_cpp;rmw_cyclonedds_cpp"
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

mkdir -p "$OUT"
exec > >(tee -a "$OUT/run.log") 2>&1
echo "=== cross-vendor abort check, $(date -Is), reps=$REPS ==="

# shellcheck disable=SC1090,SC1091 # ROS setup scripts are generated at install time and are
# not in this repository for shellcheck to follow.
for d in /opt/ros/*/setup.bash; do . "$d"; break; done
# shellcheck disable=SC1091
. "$WS/install/setup.bash"
# shellcheck disable=SC1091
. "$REPO/install/setup.bash" 2>/dev/null

echo "--- rebuilding buildfarm_perf_tests for three vendors ---"
colcon build \
  --base-paths "$WS" --build-base "$WS/build" --install-base "$WS/install" \
  --packages-select buildfarm_perf_tests \
  --cmake-args -DPERF_TEST_TOPICS="Array1k;Struct16" \
               -DPERF_TEST_RMW_IMPLEMENTATIONS="$VENDORS" \
               -DPERF_TEST_RUNTIME="10" \
               --cmake-force-configure || { echo "BUILD FAILED"; exit 1; }

for rep in $(seq 1 "$REPS"); do
    echo "--- rep $rep/$REPS $(date -Is) ---"
    rm -rf "$WS/build/buildfarm_perf_tests/test_results"
    colcon test --base-paths "$WS" --build-base "$WS/build" --install-base "$WS/install" \
        --packages-select buildfarm_perf_tests --event-handlers console_direct+ \
        --return-code-on-test-failure --ctest-args -R two_process_rmw_ \
        > "$OUT/rep${rep}.log" 2>&1
    echo "rep $rep colcon exit=$?"
    cp "$WS/build/buildfarm_perf_tests/Testing/Temporary/LastTest_"*.log "$OUT/rep${rep}.ctest.log" 2>/dev/null
done

echo "=== ATTRIBUTION ==="
python3 "$REPO/examples/perf_hil/experiments/attribute_aborts.py" "$OUT"/rep*.ctest.log
echo "=== done $(date -Is) ==="
