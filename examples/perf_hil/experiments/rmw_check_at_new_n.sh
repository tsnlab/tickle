#!/usr/bin/env bash
# rmw_shipping_check.sh at rmw_tickle's new default N = 65507 (fddad207), after regenerating every
# TickLE typesupport that the check loads.
#
# Why the regeneration is part of the check and not optional: 5bba76df appended a field
# (tickle_max_buffer_length) to rosidl_typesupport_tickle_c_message_callbacks_t. An interface
# library generated before that has the shorter struct, so rmw reads past its end and refuses the
# type with a garbage N, or silently reads 0 (TickLE Dev, 2026-09-25). performance_test's messages
# in ~/rmw_perf_ws are such a library. The generator the CMake hook runs is the pip-installed
# tickle_typesupport, which goes stale on every pull (feedback memory: pip install staleness), so
# that is reinstalled first.
#
# Steps:
#   1. pip-reinstall tools/typesupport, then confirm the installed copy has set_max_buffer_length.
#   2. Back up ~/rmw_perf_ws/{build,install}/performance_test before touching them, because
#      deleting and rebuilding performance_test broke this workspace once (2026-09-24).
#   3. Build this checkout's rmw_tickle + typesupport clean, with only /opt/ros sourced, so the
#      runner's copies cannot shadow them.
#   4. Rebuild performance_test in place against that typesupport (--cmake-force-configure), and
#      prove the generated code carries tickle_max_buffer_length = 65507.
#   5. Run rmw_shipping_check.sh unchanged.
#
# HOW TO READ IT, written before running:
#   - If step 4 finds no 65507 in the generated sources, the rebuild did not take. Stop there, and
#     treat anything after it as meaningless.
#   - A refusal "was generated for tt_MAX_BUFFER_LENGTH ..." in part 1 means a stale library was
#     loaded. That is the check working, not a TickLE bug.
#   - Part 1 must pass 4/4 with ordering counters at 0, as at N = 1472. Part 2 must show window
#     jumps in single digits, overflow 0 and out_of_order 0, as at d6d312cd. A regression at the
#     new N shows up here or nowhere, since the loopback datagrams are the same size either way.
#   - The SO_RCVBUF warning naming net.core.rmem_max is expected on this box's stock limits.
set -euo pipefail
REPO=/home/semih/tickle
WS="$HOME/rmw_perf_ws"
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then
    RIG_LOCK_SCOPE=box exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${OUT:-/tmp/tickle_rmw_check_newN_$STAMP.txt}"
exec > >(tee -a "$OUT") 2>&1
echo "=== rmw check at new N, $(date -Is), repo $(git -C "$REPO" rev-parse --short HEAD) ==="

echo "--- 1. reinstall tools/typesupport ---"
python3 -m pip install --user --break-system-packages --force-reinstall --no-deps -q "$REPO/tools/typesupport"
(cd /tmp && python3 -c "import tickle_typesupport.model as m, inspect; assert hasattr(m, 'set_max_buffer_length'), 'stale'; print('installed generator OK:', inspect.getfile(m))")

echo "--- 2. back up performance_test ---"
BK="/tmp/rmw_perf_ws_backup_$STAMP"
mkdir -p "$BK/build" "$BK/install"
cp -a "$WS/build/performance_test" "$BK/build/"
cp -a "$WS/install/performance_test" "$BK/install/"
echo "backup at $BK"

echo "--- 3. clean build of this checkout's rmw_tickle + typesupport ---"
set +u
# shellcheck disable=SC1091
. /opt/ros/lyrical/setup.bash
set -u
rm -rf "$REPO/build/rmw_tickle" "$REPO/install/rmw_tickle" \
       "$REPO/build/rosidl_typesupport_tickle_c" "$REPO/install/rosidl_typesupport_tickle_c" \
       "$REPO/build/rosidl_typesupport_tickle_cpp" "$REPO/install/rosidl_typesupport_tickle_cpp"
(cd "$REPO" && colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
    --cmake-args -DBUILD_SHARED_LIBS=ON) > "/tmp/tickle_newN_build_$STAMP.log" 2>&1 || { echo "BUILD FAILED (tickle)"; tail -20 "/tmp/tickle_newN_build_$STAMP.log"; exit 1; }
grep -h 'TICKLE_MAX_BUFFER_LENGTH' "$REPO/build/rosidl_typesupport_tickle_c/CMakeCache.txt" "$REPO/build/rmw_tickle/CMakeCache.txt" 2>/dev/null | head -3 || true

echo "--- 4. rebuild performance_test's interfaces against it ---"
set +u
# shellcheck disable=SC1091
. "$REPO/install/setup.bash"
set -u
(cd "$WS" && colcon build --packages-select performance_test --cmake-args -DCMAKE_BUILD_TYPE=Release --cmake-force-configure) \
    > "/tmp/tickle_newN_perftest_build_$STAMP.log" 2>&1 || { echo "BUILD FAILED (performance_test) - restore from $BK"; tail -20 "/tmp/tickle_newN_perftest_build_$STAMP.log"; exit 1; }
hits=$(grep -rl 'tickle_max_buffer_length' "$WS/build/performance_test" 2>/dev/null | head -50 | xargs grep -l '65507' 2>/dev/null | wc -l || true)
echo "generated performance_test sources carrying tickle_max_buffer_length and 65507: $hits"
[ "$hits" -gt 0 ] || { echo "VOID: the regenerated typesupport does not carry N=65507 - stopping"; exit 1; }

echo "--- 5. rmw_shipping_check.sh ---"
RIG_LOCK_HELD_BOX=1 "$REPO/examples/perf_hil/experiments/rmw_shipping_check.sh"
echo "=== done $(date -Is) ==="
