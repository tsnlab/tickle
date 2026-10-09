#!/usr/bin/env bash
# pc_preflight_build.sh - the PC preflight's rmw stack (rosidl_typesupport_tickle_c/_cpp, rmw_tickle, rmw_perf_pingpong,
# then performance_test against them) built from this checkout's HEAD, as the rig builds it, into a versioned
# directory outside every session directory: $PF_BUILDS/<sha12>/ (default ~/pc_preflight_builds).
#
# Why: rmw_samehost.sh's PC preflight ran ~/tickle/install, built 2026-10-07 04:30, for two days, and each failure of
# that stale build was read as one of the SHA being guarded. A build here is keyed by the commit it was built from and
# carries a fingerprint of the trees it is built from, so the preflight can pick one that matches HEAD (--find) instead
# of trusting whatever an install directory happens to hold.
#
# The rig's flags: -DBUILD_SHARED_LIBS=ON (a build without it installs the typesupports static-only, and every
# generated typesupport .so then fails to load: "no rmw_tickle typesupport ..."), CMAKE_BUILD_TYPE=None, -g -O2.
# rmw_tickle's packages and rmw_perf_pingpong in ONE colcon run (rmw_perf_pingpong's package.xml depends on the TickLE
# typesupports, so colcon orders it after them and puts them in its build environment), then performance_test from
# $PERF_TEST_SRC against that install, Release as before.
#
# The source is `git archive HEAD` (no submodules: none of these packages uses third_party/), so uncommitted changes
# in the checkout never reach a build that is named after a commit. A build is complete only when BUILD_OK exists; it
# is written last, after the libraries the preflight needs were checked to exist.
#
# Usage:
#   examples/perf_hil/experiments/pc_preflight_build.sh          build HEAD (skips if a complete build of it exists;
#                                                                 FORCE=1 rebuilds), prints the env to use
#   examples/perf_hil/experiments/pc_preflight_build.sh --find   print the newest complete build whose fingerprint
#                                                                 matches HEAD; exit 1 if there is none
#   env: PF_BUILDS(~/pc_preflight_builds) PERF_TEST_SRC(~/rmw_perf_ws/src/performance_test)
#        ROS_SETUP(/opt/ros/lyrical/setup.bash) FORCE(0) PERF(1; 0 skips performance_test - not usable by the preflight)
# Detached (a fresh build takes ~5 min): setsid nohup <this> > /path/outside/the/session.log 2>&1 < /dev/null &
# The last line is always "PC_PREFLIGHT_BUILD rc=<n> ...".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
PF_BUILDS=${PF_BUILDS:-$HOME/pc_preflight_builds}
PERF_TEST_SRC=${PERF_TEST_SRC:-$HOME/rmw_perf_ws/src/performance_test}
ROS_SETUP=${ROS_SETUP:-/opt/ros/lyrical/setup.bash}
# Every tree the four packages and the generated typesupports are built from, and this script (its flags).
FP_PATHS="src include platform tools/typesupport rmw_tickle/rmw_tickle rmw_tickle/rosidl_typesupport_tickle_c \
rmw_tickle/rosidl_typesupport_tickle_cpp rmw_tickle/rmw_perf_pingpong examples/perf_hil/experiments/pc_preflight_build.sh"

fingerprint() { # the object ids of FP_PATHS at HEAD, hashed: equal for two commits that build the same stack
    local p ids=""
    for p in $FP_PATHS; do
        ids="$ids $p=$(git -C "$REPO" rev-parse "HEAD:$p" 2>/dev/null)" || return 1
    done
    sha256sum <<<"$ids" | cut -c1-16
}

if [ "${1:-}" = --find ]; then
    fp=$(fingerprint) || { echo "pc_preflight_build: cannot fingerprint HEAD of $REPO" >&2; exit 1; }
    best="" best_t=0
    for ok in "$PF_BUILDS"/*/BUILD_OK; do
        [ -f "$ok" ] || continue
        grep -qx "fingerprint=$fp" "$ok" || continue
        t=$(stat -c %Y "$ok")
        [ "$t" -gt "$best_t" ] && best=$(dirname "$ok") best_t=$t
    done
    [ -n "$best" ] || { echo "pc_preflight_build: no complete build in $PF_BUILDS matches HEAD (fingerprint $fp)" >&2; exit 1; }
    echo "$best"
    exit 0
fi

rc=1
trap 'echo "PC_PREFLIGHT_BUILD rc=$rc dir=${D:-} $(date -Is)"' EXIT
SHA=$(git -C "$REPO" rev-parse HEAD) || { rc=2; exit; }
FP=$(fingerprint) || { echo "cannot fingerprint HEAD"; rc=2; exit; }
D=$PF_BUILDS/${SHA:0:12}
print_env() {
    echo "PF_TICKLE_INSTALL=$D/install"
    echo "PF_PERF_WS=$D/perf/install"
    echo "PF_OVERLAYS=\"$D/install/local_setup.bash $D/perf/install/local_setup.bash\""
}
if [ -f "$D/BUILD_OK" ] && [ "${FORCE:-0}" != 1 ]; then
    echo "complete build of ${SHA:0:12} exists ($D/BUILD_OK); FORCE=1 rebuilds"
    print_env
    rc=0
    exit
fi
[ "${PERF:-1}" = 1 ] && { [ -d "$PERF_TEST_SRC" ] || { echo "no performance_test source at $PERF_TEST_SRC"; rc=3; exit; }; }
mkdir -p "$PF_BUILDS" || { rc=3; exit; }
exec 9>"$PF_BUILDS/.${SHA:0:12}.lock"
flock -n 9 || { echo "another build of ${SHA:0:12} holds $PF_BUILDS/.${SHA:0:12}.lock"; rc=4; exit; }
echo "building ${SHA:0:12} (fingerprint $FP) into $D, $(date -Is)"
rm -rf "${D:?}" || { rc=3; exit; }
mkdir -p "$D/src" || { rc=3; exit; }
git -C "$REPO" archive HEAD | tar -x -C "$D/src" || { echo "git archive failed"; rc=5; exit; }

export PYTHONPATH=$D/src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}
set +u
# shellcheck disable=SC1090
source "$ROS_SETUP"
set -u
echo "stage1 rmw $(date -Is)"
(cd "$D" && colcon build --base-paths src/rmw_tickle --build-base build --install-base install \
    --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle rmw_perf_pingpong \
    --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=None '-DCMAKE_C_FLAGS=-g -O2' '-DCMAKE_CXX_FLAGS=-g -O2' \
    -DBUILD_TESTING=OFF) || { rc=11; exit; }
# The libraries the preflight loads, each one the failure mode of a wrong build: the shared typesupports (a static-only
# build has none) and rmw_perf_pingpong's own TickLE typesupport (absent when it was configured before the typesupports
# were in its environment).
need="$D/install/rmw_tickle/lib/librmw_tickle.so
$D/install/rosidl_typesupport_tickle_c/lib/librosidl_typesupport_tickle_c.so
$D/install/rosidl_typesupport_tickle_cpp/lib/librosidl_typesupport_tickle_cpp.so
$D/install/rmw_perf_pingpong/lib/librmw_perf_pingpong__rosidl_typesupport_tickle_c.so
$D/install/rmw_perf_pingpong/lib/librmw_perf_pingpong__rosidl_typesupport_tickle_cpp.so"
missing=0
while read -r f; do
    [ -f "$f" ] || { echo "MISSING after stage1: $f"; missing=1; }
done <<<"$need"
[ "$missing" = 0 ] || { rc=12; exit; }
if [ "${PERF:-1}" = 1 ]; then
    set +u
    # shellcheck disable=SC1091
    source "$D/install/local_setup.bash"
    set -u
    echo "stage2 performance_test $(date -Is)"
    (cd "$D" && colcon build --base-paths "$PERF_TEST_SRC" --build-base perf/build --install-base perf/install \
        --packages-select performance_test --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DPERFORMANCE_TEST_RT_ENABLED=ON -DPERFORMANCE_TEST_CALLBACK_EXECUTOR_ENABLED=ON) || { rc=13; exit; }
    for f in "$D/perf/install/performance_test/lib/performance_test/perf_test" \
        "$D/perf/install/performance_test/lib/libperformance_test__rosidl_typesupport_tickle_cpp.so"; do
        [ -e "$f" ] || { echo "MISSING after stage2: $f"; missing=1; }
    done
    [ "$missing" = 0 ] || { rc=14; exit; }
    printf 'sha=%s\nfingerprint=%s\nbuilt=%s\nperf_test_src=%s\nros_setup=%s\n' "$SHA" "$FP" "$(date -Is)" \
        "$PERF_TEST_SRC" "$ROS_SETUP" >"$D/BUILD_OK"
    print_env
else
    echo "PERF=0: performance_test not built, so no BUILD_OK (the preflight cannot use this build)"
fi
rc=0
