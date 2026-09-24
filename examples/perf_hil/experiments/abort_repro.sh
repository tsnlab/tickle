#!/usr/bin/env bash
# Reproduce and instrument the "Data consistency violated" abort, at the user's own direction to
# chase it (2026-09-24). This runs on the dev box deliberately and does not contradict the
# no-performance-on-the-PC decision: nothing here is a performance figure. It asks whether a
# best-effort stream delivers samples out of order or twice, which is a correctness question and
# exactly what this machine is for.
#
# What is already established, so this does not re-litigate it:
#   - the abort predates the socket split (LastTest_20260923-1203.log, 21:05, 10 occurrences, all
#     four cells; 82a6a02d landed 23:16)
#   - the reliable path is not involved: "Writer proxy created" appears ZERO times in both the
#     pre-split and post-split aborting runs, and perf_test runs --keep_last with no --reliable, so
#     no ACKNACK and no retransmission. A recovered-sample-out-of-order explanation is excluded.
# So: a plain best-effort stream, one publisher, one subscriber, on loopback, is delivering samples
# out of order or twice, with no protocol machinery in the way.
#
# Phase 1 (this script): what is the reproduction rate, and does the WIRE show the disorder?
# The capture is the point. If the wire is ordered and delivery is not, the reordering is in the
# reader - which is the two-socket alternation, and Dev's discriminator is then the next arm. If
# the wire itself carries duplicates or out-of-order sequence, the reader is exonerated and the
# cause is upstream of it. Neither can be told from the abort message alone, and a capture needs no
# code change in core or in the harness.
set -euo pipefail

: "${ROS_DISTRO_NAME:?set ROS_DISTRO_NAME (e.g. lyrical)}"
RMW_PERF_WS="${RMW_PERF_WS:-$HOME/rmw_perf_ws}"
REPS="${REPS:-10}"
CELL="${CELL:-two_process_rmw_tickle_sync_Struct16}"
OUT="${OUT:-/tmp/tickle_abort_repro.txt}"
CAPDIR="${CAPDIR:-/tmp/tickle_abort_caps}"
mkdir -p "$CAPDIR"

# Match rmw-perf.yml, which sets this and which is the condition the abort was observed under. The
# first version of this script did not, so it ran on the compiled-in 255.255.255.255 default - which
# has no subnet to be scoped by and follows the default route, i.e. out ens18 onto the shared lab
# network. Two consequences, both found by looking at the capture rather than by reasoning: the runs
# were not reproducing CI's conditions, and a capture on `lo` could not see the broadcast half of
# the stream at all, because the broadcast half was never on lo. Roughly half of each run was
# invisible to the instrument.
export TICKLE_BROADCAST_ADDR="${TICKLE_BROADCAST_ADDR:-127.255.255.255}"

: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== abort reproduction, $CELL, $REPS reps, $(date -Is) ==="

# ROS 2's own setup.bash reads AMENT_TRACE_SETUP_FILES unguarded, so it dies under `set -u`.
# compare_rmw_perf.sh solves this by running without -u at all; scoping it to the two sources keeps
# the guard over this script's own code, where an unset variable is a bug rather than a convention.
set +u
# shellcheck disable=SC1090,SC1091 # both depend on this box's own ROS_DISTRO_NAME/RMW_PERF_WS
source "/opt/ros/$ROS_DISTRO_NAME/setup.bash"
# shellcheck disable=SC1091
source "$RMW_PERF_WS/install/setup.bash"
set -u

aborts=0
for rep in $(seq 1 "$REPS"); do
    cap="$CAPDIR/rep${rep}.pcap"
    # The capture is optional because it needs CAP_NET_RAW, which this box does not grant
    # unprivileged. Without it the reproduction RATE is still measured, which is what decides
    # whether the question is answerable at all; the wire evidence needs the permission and is
    # requested separately rather than worked around. CAPTURE=0 skips it explicitly.
    capture=1
    [ "${CAPTURE:-1}" = "1" ] || capture=0
    # Loopback capture of everything on TickLE's well-known port AND every ephemeral data port, so
    # both halves of a split stream are in one file with one clock. -U so the file reflects what
    # has been captured rather than what has been flushed.
    tcpdump_pid=""
    if [ "$capture" = 1 ]; then
        tcpdump -U -i lo -n -s 200 -w "$cap" 'udp' >"$CAPDIR/rep${rep}.tcpdump.err" 2>&1 &
        tcpdump_pid=$!
        sleep 1
        if ! grep -q "listening on" "$CAPDIR/rep${rep}.tcpdump.err"; then
            say "rep $rep: capture did not start, continuing without it:"
            say "  $(head -2 "$CAPDIR/rep${rep}.tcpdump.err" | tr '\n' ' ')"
            kill "$tcpdump_pid" 2>/dev/null || true
            tcpdump_pid=""
            capture=0
        fi
    fi

    rm -rf "$RMW_PERF_WS/build/buildfarm_perf_tests/test_results"
    set +e
    colcon test --base-paths "$RMW_PERF_WS" --build-base "$RMW_PERF_WS/build" \
        --install-base "$RMW_PERF_WS/install" --packages-select buildfarm_perf_tests \
        --return-code-on-test-failure --ctest-args -R "$CELL" >"$CAPDIR/rep${rep}.colcon.log" 2>&1
    rc=$?
    set -e
    if [ -n "$tcpdump_pid" ]; then
        kill "$tcpdump_pid" 2>/dev/null || true
        wait "$tcpdump_pid" 2>/dev/null || true
    fi

    # Scan EVERY launch log this rep produced, not one named after $CELL: $CELL is a ctest regex and
    # may match the whole matrix. It usually should - 0 of 10 reps aborted when sync/Struct16 was run
    # alone, while the CI job that aborted ran all four cells in one invocation, and the 21:05
    # pre-split log carries 10 occurrences spread across all four. Whatever this is, running the
    # cell in isolation is not the condition that produces it.
    # Preserve evidence for ANY failing rep, not only an aborting one. The first version kept logs
    # only when "Data consistency violated" appeared - and 4 of 12 reps failed the suite while only
    # 2 aborted, so the evidence for the other two failure mode(s) was deleted by this script before
    # anyone could look at it. Keeping it only for the failure you already understand is how a
    # second failure mode stays invisible.
    hit=""
    other=""
    for log in "$RMW_PERF_WS/build/buildfarm_perf_tests/launch_test/"*two_process_rmw_*.txt; do
        [ -f "$log" ] || continue
        if grep -aq "Data consistency violated" "$log"; then
            hit="$hit $(basename "$log" .txt)"
        fi
    done
    if [ "$rc" -ne 0 ]; then
        mkdir -p "$CAPDIR/rep${rep}.evidence"
        cp -r "$RMW_PERF_WS/build/buildfarm_perf_tests/launch_test/"*two_process_rmw_*.txt \
              "$RMW_PERF_WS/build/buildfarm_perf_tests/test_results/" "$CAPDIR/rep${rep}.evidence/" 2>/dev/null || true
        # Name every cell whose xunit reports a failure, so a second failure mode is visible in the
        # summary rather than only in a directory nobody opens.
        for x in "$CAPDIR/rep${rep}.evidence"/*/*.xunit.xml "$CAPDIR/rep${rep}.evidence"/*.xunit.xml; do
            [ -f "$x" ] || continue
            grep -q 'failures="0"' "$x" || other="$other $(basename "$x" .xunit.xml)"
        done
    fi
    # The instrument has to be shown complete before a capture is used as evidence: with the
    # broadcast address above, BOTH halves of the stream are on lo, so a capture with no broadcast
    # datagrams in it is a capture that missed half the run and must not be read as one that saw an
    # ordered wire.
    if [ "$capture" = 1 ] && [ -f "$cap" ]; then
        bcast=$(tcpdump -r "$cap" -n 'udp and dst host 127.255.255.255' 2>/dev/null | wc -l)
        say "rep $rep: capture holds $bcast broadcast datagram(s)$([ "$bcast" -eq 0 ] && echo '  <-- INSTRUMENT INCOMPLETE, do not read this capture as evidence of an ordered wire')"
    fi
    [ -n "$other" ] && say "rep $rep: cells reporting a failure:$other"
    if [ -n "$hit" ]; then
        aborts=$((aborts + 1))
        say "rep $rep: ABORTED in$hit (colcon rc=$rc)$([ "$capture" = 1 ] && echo " - capture at $cap")"
    else
        if [ "$rc" -ne 0 ]; then say "rep $rep: FAILED but did not abort (colcon rc=$rc) - evidence in $CAPDIR/rep${rep}.evidence"; else say "rep $rep: clean"; fi
        [ "$rc" -eq 0 ] && rm -f "$cap" "$CAPDIR/rep${rep}.tcpdump.err"
    fi
done

say ""
say "=== $aborts of $REPS reps aborted; captures for the aborting reps are in $CAPDIR ==="
