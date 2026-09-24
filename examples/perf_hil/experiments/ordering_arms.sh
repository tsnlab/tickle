#!/usr/bin/env bash
# Does the ordering work actually do anything, and does anything notice when it does not?
#
# Both halves of the 2026-09-24 ordering work are only observable when they FAIL. With the
# BEST_EFFORT discard and the RELIABLE reorder buffer working, the application never sees a sample
# out of order - so every detector for that condition reads zero, and "the detector is correct" is
# indistinguishable from "the detector is dead code". Two things were in that state when this was
# written:
#
#   * rmw_tickle's reorder buffer: reorder_held_peak == 0 on every clean run, which reads the same
#     whether the buffer is holding nothing or is not wired up at all.
#   * performance_test's patch (f) log line: verified to be READ by the gate (against a fabricated
#     log) and never once observed being WRITTEN by the binary.
#
# So the arms differ by one -D on the same commit, and loss is injected so there is something to
# order in the first place.
#
# HOW TO READ THE RESULT - written down before running, so the answer cannot be chosen afterwards:
#
#   Arm A (ordering ON, loss injected)
#     reorder_held_peak > 0   -> the buffer genuinely holds samples under loss. THE positive result.
#     reorder_held_peak == 0  -> the wiring does not do what core's unit tests say core does.
#     out_of_order == 0       -> ordering held while it was being stressed.
#     out_of_order_discarded > 0 on the BEST_EFFORT cell -> the discard fired.
#
#   Arm B (ordering OFF, same loss)
#     out_of_order > 0        -> the defect is genuinely back, so this arm is live.
#     TICKLE-PATCH-F line present -> the detector works. This is the only way to learn that.
#     line absent WITH out_of_order > 0 -> patch (f) is dead code and its gate is decorative.
#
#   ANY arm with delivered much below the ~10000 a cell publishes is VOID, not a result. Injected
#   loss also hits discovery, so a pair that never matched delivers almost nothing and looks
#   exactly like a buffer failure. Read delivered= before reading anything else.
#
# Loss is injected inside TickLE (TT_RX_DROP_PERCENT, hal_linux.c) rather than with tc on lo, so it
# cannot reach another session's benchmark running on this same box and leaves no global state
# behind to restore.
set -o pipefail

WS="$HOME/rmw_perf_ws"; REPO=/home/semih/tickle-dev
OUT="${OUT:-/tmp/ordering_arms}"     # outside any session scratchpad on purpose
DROP="${DROP:-8}"
mkdir -p "$OUT"
exec > >(tee -a "$OUT/run.log") 2>&1
echo "=== ordering arms, drop=${DROP}%, $(date -Is) ==="

# shellcheck disable=SC1090,SC1091 # ROS setup scripts are generated at install time.
for d in /opt/ros/*/setup.bash; do . "$d"; break; done
# shellcheck disable=SC1091
. "$WS/install/setup.bash"

run_arm() {
    local arm="$1" extra="$2" reliable="$3"
    echo "--- building arm $arm (CMAKE_C_FLAGS=$extra) ---"
    cd "$REPO" || return 1
    colcon build --packages-select rmw_tickle rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp \
        --cmake-args -DBUILD_SHARED_LIBS=ON "-DCMAKE_C_FLAGS=$extra" > "$OUT/build_$arm.log" 2>&1 \
        || { echo "arm $arm BUILD FAILED"; return 1; }
    # shellcheck disable=SC1091
    . "$REPO/install/setup.bash"
    export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=127.255.255.255
    local pt="$WS/install/performance_test/lib/performance_test/perf_test"
    local common=(-c ROS2 -t Array1k --max_runtime 15 --keep_last --history_depth 10)
    [ "$reliable" = "reliable" ] && common+=(--reliable)
    # -p/-s BEFORE --ros-args: perf_test flags after a bare --ros-args are parsed as ROS parameter
    # overrides and the node dies on "Couldn't parse parameter override rule".
    local rosargs=(--ros-args --param start_type_description_service:=false)
    echo "--- running arm $arm ($reliable) ---"
    TICKLE_NODE_ID=101 "$pt" "${common[@]}" -p 1 -s 0 "${rosargs[@]}" > "$OUT/${arm}_pub.log" 2>&1 &
    local pub=$!
    TICKLE_NODE_ID=102 "$pt" "${common[@]}" -p 0 -s 1 "${rosargs[@]}" > "$OUT/${arm}_sub.log" 2>&1 &
    local sub=$!
    wait $pub 2>/dev/null; wait $sub 2>/dev/null
    echo "=== ARM $arm ($reliable) ==="
    grep -E 'delivery:' "$OUT/${arm}_sub.log" | tail -1
    echo -n "  patch-(f) lines: "; grep -ci 'TICKLE-PATCH-F\|not strictly' "$OUT/${arm}_sub.log" || true
    grep -i 'TICKLE-PATCH-F\|Data consistency' "$OUT/${arm}_sub.log" | head -2
}

run_arm A_reliable   "-DTT_RX_DROP_PERCENT=$DROP" reliable
run_arm A_besteffort "-DTT_RX_DROP_PERCENT=$DROP" besteffort
run_arm B_besteffort "-DTT_RX_DROP_PERCENT=$DROP -DTT_ORDERING_DISABLED=1" besteffort
run_arm B_reliable   "-DTT_RX_DROP_PERCENT=$DROP -DTT_ORDERING_DISABLED=1" reliable

echo "--- restoring the default build ---"
cd "$REPO" && colcon build --packages-select rmw_tickle rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp \
    --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_C_FLAGS= > "$OUT/build_restore.log" 2>&1
echo "restore exit=$?"
echo "=== done $(date -Is) ==="
