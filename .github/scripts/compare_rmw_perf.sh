#!/usr/bin/env bash
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# One-off, run-by-hand comparison of rmw_tickle against rmw_fastrtps_cpp/rmw_cyclonedds_cpp via
# ros2/buildfarm_perf_tests, each DDS vendor forced onto real UDPv4 (no shared-memory transport)
# so the same-host comparison is fair - see rmw_tickle/PLAN.md's own Milestone 14 for why that
# forcing is necessary in the first place (same-host shared-memory transport otherwise gives
# FastDDS/CycloneDDS an unfair advantage rmw_tickle structurally can't have).
#
# Deliberately NOT wired into any GitHub Actions workflow (the user's own call, 2026-09-19,
# after "TickLE Plan" had started integrating this into rmw-perf.yml - a standalone script run
# by hand, with results copied into rmw_tickle/COMPARISON.MD afterward, is enough for something
# this intermittent). Run this directly on a box with buildfarm_perf_tests already provisioned
# per .github/scripts/README-rmw-perf.md (today, that's the tickle-perf self-hosted runner's own
# ~/rmw_perf_ws - SSH there and run this script from a checkout of this repo).
#
# Usage: .github/scripts/compare_rmw_perf.sh [runtime_seconds]
#   runtime_seconds: per topic/rmw/sync-mode benchmark duration (default 10, matching rmw-perf.
#                    yml's own PERF_TEST_RUNTIME - see that workflow's own comment on why it's
#                    short by buildfarm_perf_tests' own 30s default).

# No -u: /opt/ros/*/setup.bash itself references AMENT_TRACE_SETUP_FILES without ever setting it
# first - not nounset-safe, a real ROS 2/ament-wide quirk, not something to work around here.
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUNTIME="${1:-10}"

: "${RMW_PERF_WS:="$HOME/rmw_perf_ws"}"
: "${ROS_DISTRO_NAME:?Set ROS_DISTRO_NAME to the real ROS 2 distro installed on this box (not necessarily jazzy - see rmw-perf.yml for why)}"

# Keep TickLE's discovery traffic on this box (2026-09-23, a real incident, not a precaution):
# hal_linux.c's compiled-in default is _tt_NODE_BROADCAST 255.255.255.255 (config.h), which on this
# machine routes out ens18 onto the lab network - the same L2 segment the rig Pis sit on via wlan0.
# That flooded a shared network with discovery traffic (a UDP broadcast is delivered to every port
# on the segment and, on Wi-Fi, goes out at the lowest basic rate), and it cross-contaminated
# measurements in both directions: the rig's own liveliness sweeps recorded departures of node 101,
# which is this benchmark's publisher, and this benchmark's logs recorded "Node 2/3 presumed dead",
# which are the Pis. Both processes of this benchmark run on this one box, so loopback broadcast is
# not a workaround - it is the correct scope for a same-host benchmark.
export TICKLE_BROADCAST_ADDR="${TICKLE_BROADCAST_ADDR:-127.255.255.255}"
# Second, independent barrier for the two DDS vendors: their profiles beside this script pin them
# to loopback, and this puts them on a domain nothing else here uses. Domain and interface fail
# differently - a collision with someone else's ROS 2 work on domain 0 is as likely as an
# interface one - so neither alone is enough.
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-77}"

PERF_TEST_TOPICS="Array1k;Struct16"
PERF_TEST_RMW_IMPLEMENTATIONS="rmw_tickle;rmw_fastrtps_cpp;rmw_cyclonedds_cpp"

echo "=== Building rmw_tickle against the pre-provisioned ROS 2 + DDS-vendor underlay ==="
# shellcheck disable=SC1090,SC1091 # both paths depend on this box's own ROS_DISTRO_NAME/RMW_PERF_WS
source "/opt/ros/$ROS_DISTRO_NAME/setup.bash"
# shellcheck disable=SC1091
source "$RMW_PERF_WS/install/setup.bash"
(
  cd "$REPO_ROOT"
  # TICKLE_EXTRA_CFLAGS lets an investigation rebuild core with a different compile-time constant
  # without editing config.h - e.g. TICKLE_EXTRA_CFLAGS=-Dtt_UNICAST_PEER_THRESHOLD=0 to force every
  # send to go out as a broadcast.
  colcon build \
    --packages-select rmw_tickle rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp \
    --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_C_FLAGS="${TICKLE_EXTRA_CFLAGS:-}" \
    ${TICKLE_EXTRA_CFLAGS:+--cmake-force-configure}
)

# Verify the extra flags actually reached the compiler rather than trusting colcon's exit code:
# without --cmake-force-configure colcon reuses the existing cache and reports success having
# changed nothing (found 2026-09-23, after a whole 6-run A/B was invalidated by exactly that).
if [ -n "${TICKLE_EXTRA_CFLAGS:-}" ]; then
  # Check the CMake cache first: it is written by every configure, while compile_commands.json is
  # only emitted when CMAKE_EXPORT_COMPILE_COMMANDS produces one - a worktree build without it made
  # this guard abort a correct run (2026-09-23). Fail only when NEITHER shows the flag.
  if ! grep -q -- "$TICKLE_EXTRA_CFLAGS" "$REPO_ROOT/build/rmw_tickle/CMakeCache.txt" 2>/dev/null &&
     ! grep -q -- "$TICKLE_EXTRA_CFLAGS" "$REPO_ROOT/build/rmw_tickle/compile_commands.json" 2>/dev/null; then
    echo "TICKLE_EXTRA_CFLAGS=$TICKLE_EXTRA_CFLAGS did not reach the compile commands - aborting" >&2
    exit 1
  fi
  echo "verified: $TICKLE_EXTRA_CFLAGS is in rmw_tickle's compile commands"
fi

echo "=== Rebuilding buildfarm_perf_tests with all three rmws visible ==="
# shellcheck disable=SC1091 # generated by the colcon build above, doesn't exist at lint time
source "$REPO_ROOT/install/setup.bash"
colcon build \
  --base-paths "$RMW_PERF_WS" \
  --build-base "$RMW_PERF_WS/build" \
  --install-base "$RMW_PERF_WS/install" \
  --packages-select buildfarm_perf_tests \
  --cmake-args \
    -DPERF_TEST_TOPICS="$PERF_TEST_TOPICS" \
    -DPERF_TEST_RMW_IMPLEMENTATIONS="$PERF_TEST_RMW_IMPLEMENTATIONS" \
    -DPERF_TEST_RUNTIME="$RUNTIME" \
  --cmake-force-configure

echo "=== Forcing FastDDS/CycloneDDS onto real UDPv4 (no shared-memory transport) ==="
export FASTRTPS_DEFAULT_PROFILES_FILE="$SCRIPT_DIR/fastdds_udp_only.xml"
export CYCLONEDDS_URI="file://$SCRIPT_DIR/cyclonedds_no_shm.xml"
echo "FASTRTPS_DEFAULT_PROFILES_FILE=$FASTRTPS_DEFAULT_PROFILES_FILE"
echo "CYCLONEDDS_URI=$CYCLONEDDS_URI"

echo "=== Sanity check before trusting any timing number below - read this by hand ==="
echo "--- /dev/shm contents before the benchmark runs ---"
ls -la /dev/shm/ || true
echo "--- resolved transport config ---"
cat "$FASTRTPS_DEFAULT_PROFILES_FILE"

echo "=== Running the benchmark (two-process, real rclcpp/rmw boundary, all three rmws) ==="
rm -rf "$RMW_PERF_WS/build/buildfarm_perf_tests/test_results"
# `|| true` on both, matching rmw-perf.yml's own established pattern: this benchmark's own
# numbers are the actual point of running it, not a pass/fail gate - a real per-combo failure
# (e.g. a crash) still needs the summary table below to run so it's visible, not abort the script
# right here before ever printing it.
colcon test \
  --base-paths "$RMW_PERF_WS" \
  --build-base "$RMW_PERF_WS/build" \
  --install-base "$RMW_PERF_WS/install" \
  --packages-select buildfarm_perf_tests \
  --event-handlers console_direct+ \
  --ctest-args -R two_process_rmw_ || true
colcon test-result --test-result-base "$RMW_PERF_WS/build" --verbose || true

echo "=== Summary (copy the table below into rmw_tickle/COMPARISON.MD by hand) ==="
python3 "$SCRIPT_DIR/rmw_perf_summary.py" \
  "$RMW_PERF_WS/build/buildfarm_perf_tests/test_results/buildfarm_perf_tests"
