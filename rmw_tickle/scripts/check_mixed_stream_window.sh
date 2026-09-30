#!/usr/bin/env bash
# Two questions in one run of check_ros2_interfaces -r, because the second only means anything if the
# first says yes.
#
# 1. Did this process load the libraries under test? Until 2026-09-30 this was read from
#    /proc/<pid>/maps by the harness, which can only do it while the process is still alive - and in
#    -r mode the publisher's poll used its whole timeout first, so the subscriber was read after it
#    had exited and identity came back "not seen". Each program now prints what it mapped and the
#    harness reads that, so the answer no longer depends on when anyone looked.
#
# 2. Is the oversize mixed-stream window - a same-host stream whose big samples go out over UDP while
#    its small ones go through shared memory - something this build actually enters?
#
# Pre-registered, so the run cannot be read to taste afterwards:
#
#   identity: OK (both sides)     the run's counters are admissible; go on to question 2
#   identity: WRONG LIBRARY       a real shadowing; the counters below belong to someone else's build
#                                 and say nothing about this one. Fix the environment, not the code.
#   identity: not reported        the process never got far enough to say, or the reporting is not in
#                                 the binary. A different fault from loading the wrong file, and not
#                                 evidence either way about shadowing.
#
#   tx_udp_oversize > 0 while tx_shm > 0   the window is real and this build enters it: SHM_PLAN 6c
#                                          describes a behaviour that happens, and it needs a test.
#   tx_udp_oversize = 0 while tx_shm > 0   the seam is there but nothing measured exercises it: 6c
#                                          must say "structural but unexercised" rather than imply
#                                          a measured effect.
#   tx_shm = 0                             VOID. Nothing went through shared memory at all, so this
#                                          run cannot speak about a mixed stream. Do not read the
#                                          oversize zero as an answer - it is an absence of the test.
#
# Its own log lives outside any session directory, because the result is the deliverable and has to
# outlive whoever started it.
set -uo pipefail

REPO=/home/semih/tickle-dev
OUT="${1:-/tmp/mixed_stream_window.log}"
KEPT="${OUT%.log}_process_logs"
WORKSPACE="${HOME}/tickle_ros2_interfaces"
# About 90 s: ~25 s to build the check package into its own temp workspace, ~35 s for the run. The
# 1.5x deadline is what stops a wait that can never end from being mistaken for work in progress.
DEADLINE=135

{
    echo "=== check_mixed_stream_window $(date -Is) ==="
    # Whichever distro is actually installed, and fatal if none is: sourcing /opt/ros/jazzy when this
    # machine has lyrical failed in the run's first second on 2026-09-30, and because the sourcing was
    # not checked the script carried on and produced a log that looked like a real attempt.
    ros_setup=""
    for distro in /opt/ros/*/setup.bash; do
        [ -f "$distro" ] && ros_setup="$distro" && break
    done
    if [ -z "$ros_setup" ]; then
        echo "FATAL: no /opt/ros/*/setup.bash - nothing to source, so nothing below would mean anything"
        exit 2
    fi
    echo "ROS: $ros_setup"
    set +u
    # shellcheck disable=SC1090,SC1091
    . "$ros_setup"
    # shellcheck disable=SC1091
    . "$REPO/install/setup.bash"
    set -u
    [ -n "${ROS_DISTRO:-}" ] || {
        echo "FATAL: ROS_DISTRO unset after sourcing $ros_setup"
        exit 2
    }
    echo "rmw_tickle on AMENT_PREFIX_PATH: $(echo "${AMENT_PREFIX_PATH:-}" | tr ':' '\n' | grep -m1 rmw_tickle || echo NONE)"
    export RMW_IMPLEMENTATION=rmw_tickle
    # The counters live in each process's own log, not in the check's summary: the traffic line is
    # printed by node_destroy_locked, and the check only tails a few lines of each process. Reading
    # the summary instead is what made the first attempt at this report VOID with the numbers sitting
    # in a file next door.
    export CHECK_ROS2_KEEP_LOGS="$KEPT"
    rm -rf "$KEPT"
    timeout "$DEADLINE" "$REPO/rmw_tickle/scripts/check_ros2_interfaces.sh" -r -w "$WORKSPACE"
    rc=$?
    echo "=== check_ros2_interfaces -r exit $rc at $(date -Is) ==="
    if [ "$rc" = 124 ]; then
        echo "VERDICT: TIMED OUT at ${DEADLINE}s - 1.5x the estimate. Find the cause before rerunning."
    fi
} >"$OUT" 2>&1

# The counters, read back out of what the run itself printed. Gathered into a variable first and
# appended in one go: reading a file inside a block that is appending to it reads a moving target.
identity_lines=$(grep -c "^identity: OK\|^publisher identity: OK" "$OUT" || true)
traffic_lines=$(grep -lE 'tx_udp_oversize=[0-9]+' "$KEPT"/*.txt 2>/dev/null | wc -l)
counts=$(grep -ohE 'tx_shm=[0-9]+|tx_udp_oversize=[0-9]+|out_of_order_discarded=[0-9]+' "$KEPT"/*.txt 2>/dev/null |
    sort | uniq -c || true)
segments=$(sed -n 's/^segment-self [^:]*: //p' "$KEPT"/*.txt 2>/dev/null | sort -u | tr '\n' ' ')
shm=$(grep -ohE 'tx_shm=[0-9]+' "$KEPT"/*.txt 2>/dev/null | cut -d= -f2 | sort -rn | head -1)
over=$(grep -ohE 'tx_udp_oversize=[0-9]+' "$KEPT"/*.txt 2>/dev/null | cut -d= -f2 | sort -rn | head -1)

if [ "$identity_lines" != 2 ]; then
    verdict="VERDICT: counters not admissible - identity did not read OK on both sides ($identity_lines of 2)"
elif [ "$traffic_lines" = 0 ]; then
    verdict="VERDICT: NO INSTRUMENT - not one process log carries a traffic line, so there is no reading here to call zero"
elif [ -z "${shm:-}" ] || [ "${shm:-0}" = 0 ]; then
    verdict="VERDICT: VOID - no traffic went through shared memory, so this run says nothing about a mixed stream"
elif [ "${over:-0}" = 0 ]; then
    verdict="VERDICT: window STRUCTURAL BUT UNEXERCISED - tx_shm=$shm, tx_udp_oversize=0"
else
    verdict="VERDICT: window REAL AND ENTERED - tx_shm=$shm, tx_udp_oversize=$over"
fi

{
    echo "=== mixed-stream window, from this run's own traffic lines ==="
    echo "identity lines reading OK: $identity_lines of 2 expected"
    echo "process logs carrying a traffic line: $traffic_lines"
    echo "segments mapped across all processes: ${segments:-none}"
    echo "$counts"
    echo "$verdict"
} >>"$OUT"
echo "$verdict"
