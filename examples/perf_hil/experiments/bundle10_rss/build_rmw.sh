#!/usr/bin/env bash
# build_rmw.sh - the rmw_tickle build bundle10_rss measures: rmw_tickle and its two typesupports from this checkout
# (shared libraries, as rmw-perf.yml builds them) plus the rss_tables_rmw node package, RelWithDebInfo so gdb can read
# the struct layouts (layout.py). Into WS (default /home/semih/tmp_build/b10rss/ws) with only /opt/ros/lyrical
# sourced: another workspace on AMENT_PREFIX_PATH would shadow this rmw_tickle.
#
# Usage: [WS=<dir>] build_rmw.sh      (nice -n 10, one package at a time, make -j2: the PC is shared)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
WS=${WS:-/home/semih/tmp_build/b10rss/ws}
mkdir -p "$WS"
# shellcheck disable=SC2016 # expanded by the inner shell
exec env -i PATH=/usr/bin:/bin HOME="$HOME" TMPDIR=/home/semih/tmp_build MAKEFLAGS=-j2 bash -c '
set -eo pipefail
source /opt/ros/lyrical/setup.bash
cd "$1"
nice -n 10 colcon --log-base "$1/log" build --base-paths "$2/rmw_tickle" "$3" \
  --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle rss_tables_rmw \
  --build-base "$1/build" --install-base "$1/install" --parallel-workers 1 --event-handlers console_direct- \
  --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
echo BUILD_RMW_DONE
' _ "$WS" "$REPO" "$HERE/rss_tables_rmw"
