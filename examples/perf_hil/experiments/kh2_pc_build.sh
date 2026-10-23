#!/usr/bin/env bash
# kh2_pc_build.sh - the rmw stack (typesupports, rmw_tickle, rmw_perf_pingpong) of one commit, built for a PC A/B of
# the KH2 receive return (ROADMAP "Other open items"), as pc_preflight_build.sh builds it but with extra C flags (the
# libc variant, -DTT_HAL_LIBC_CALLS, which sysstamp needs) and into a directory named by the caller.
#
# The source is `git archive <rev>`, so uncommitted edits never reach a build named after a commit.
# Usage: kh2_pc_build.sh <out dir> <rev> ['<extra C flags>']     (~4 min; detach it: setsid nohup ... &)
# The last line is always "KH2_BUILD_END rc=<n> ...".
set -uo pipefail
D=${1:?out dir}
REV=${2:?rev}
XF=${3:-}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
rc=1
trap 'echo "KH2_BUILD_END rc=$rc dir=$D rev=$REV flags=[$XF] $(date -Is)"' EXIT
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
ulimit -v 4194304
SHA=$(git -C "$REPO" rev-parse --verify -q "$REV^{commit}") || { rc=2; exit; }
rm -rf "${D:?}"
mkdir -p "$D/src" || { rc=3; exit; }
git -C "$REPO" archive "$SHA" | tar -x -C "$D/src" || { rc=5; exit; }
export PYTHONPATH=$D/src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}
set +u
# shellcheck disable=SC1091
source /opt/ros/lyrical/setup.bash
set -u
(cd "$D" && nice -n 10 colcon build --parallel-workers 2 --base-paths src/rmw_tickle --build-base build \
    --install-base install \
    --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle rmw_perf_pingpong \
    --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=None "-DCMAKE_C_FLAGS=-g -O2 $XF" '-DCMAKE_CXX_FLAGS=-g -O2' \
    -DBUILD_TESTING=OFF) || { rc=11; exit; }
[ -f "$D/install/rmw_tickle/lib/librmw_tickle.so" ] || { rc=12; exit; }
echo "sha=$SHA" >"$D/BUILD_SHA"
rc=0
