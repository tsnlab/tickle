#!/usr/bin/env bash
# rmw_bisect_build.sh - the rmw stack (TickLE typesupports, rmw_tickle, rmw_perf_pingpong) of ANY commit, built as
# pc_preflight_build.sh's stage 1 builds HEAD (the rig's flags: shared, CMAKE_BUILD_TYPE=None, -g -O2), into
# $BISECT_BUILDS/<sha12>/install. For a bisect over commits older than pc_preflight_build.sh itself (2026-10-09).
# The source is `git archive <sha>`, so nothing uncommitted reaches it. BUILD_OK is written last.
# Usage: rmw_bisect_build.sh <commit>    env: BISECT_BUILDS (~/rmw_bisect_builds), ROS_SETUP (/opt/ros/lyrical)
# Last line always "RMW_BISECT_BUILD rc=<n> sha=<sha12>".
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BISECT_BUILDS=${BISECT_BUILDS:-$HOME/rmw_bisect_builds}
ROS_SETUP=${ROS_SETUP:-/opt/ros/lyrical/setup.bash}
rc=1
SHA=$(git -C "$REPO" rev-parse --verify -q "${1:?usage: rmw_bisect_build.sh <commit>}^{commit}") || {
    echo "RMW_BISECT_BUILD rc=2 sha=-"
    exit 2
}
D=$BISECT_BUILDS/${SHA:0:12}
trap 'echo "RMW_BISECT_BUILD rc=$rc sha=${SHA:0:12} $(date -Is)"' EXIT
[ -f "$D/BUILD_OK" ] && { echo "complete build exists: $D"; rc=0; exit; }
if ! { rm -rf "${D:?}" && mkdir -p "$D/src"; }; then rc=3; exit; fi
git -C "$REPO" archive "$SHA" | tar -x -C "$D/src" || { rc=5; exit; }
export PYTHONPATH=$D/src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}
set +u
# shellcheck disable=SC1090
source "$ROS_SETUP"
set -u
(cd "$D" && colcon build --base-paths src/rmw_tickle --build-base build --install-base install \
    --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle rmw_perf_pingpong \
    --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=None '-DCMAKE_C_FLAGS=-g -O2' '-DCMAKE_CXX_FLAGS=-g -O2' \
    -DBUILD_TESTING=OFF) || { rc=11; exit; }
# Before 8c9edaf4, rmw_perf_pingpong's package.xml did not depend on the TickLE typesupports, so colcon may configure
# it before they exist and it then has no TickLE typesupport. Rebuild it alone, from a clean cache, with them sourced.
if [ ! -e "$D/install/rmw_perf_pingpong/lib/librmw_perf_pingpong__rosidl_typesupport_tickle_cpp.so" ]; then
    echo "rmw_perf_pingpong has no TickLE typesupport: rebuilding it with the typesupports sourced"
    set +u
    # shellcheck disable=SC1091
    source "$D/install/local_setup.bash"
    set -u
    rm -rf "$D/build/rmw_perf_pingpong" "$D/install/rmw_perf_pingpong"
    (cd "$D" && colcon build --base-paths src/rmw_tickle --build-base build --install-base install \
        --packages-select rmw_perf_pingpong \
        --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=None '-DCMAKE_C_FLAGS=-g -O2' '-DCMAKE_CXX_FLAGS=-g -O2' \
        -DBUILD_TESTING=OFF) || { rc=13; exit; }
fi
for f in rmw_tickle/lib/librmw_tickle.so rmw_perf_pingpong/lib/rmw_perf_pingpong/ping_node \
    rmw_perf_pingpong/lib/librmw_perf_pingpong__rosidl_typesupport_tickle_cpp.so; do
    [ -e "$D/install/$f" ] || { echo "MISSING $D/install/$f"; rc=12; exit; }
done
printf 'sha=%s\nbuilt=%s\n' "$SHA" "$(date -Is)" >"$D/BUILD_OK"
rc=0
