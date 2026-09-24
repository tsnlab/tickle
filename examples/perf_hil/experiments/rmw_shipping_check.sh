#!/usr/bin/env bash
# Does the build that ships work through ROS 2 (rmw_tickle), not only through TickLE's own harness?
#
# Everything about RELIABLE ordering on 2026-09-24 - the O(1) reorder buffer, strict order, the
# buffer-overrun fix - was verified on the native perf_hil harness. rmw_tickle has its own reorder
# buffer wiring, which was broken once that day (storage attached, then reset by
# tt_Node_create_subscriber()), and the CI job that exercised the live two-process rmw path became
# dispatch-only the same day. So nothing currently checks what users actually run.
#
# Three parts:
#   1. the buildfarm two_process_rmw_ matrix on the shipping build - does the product work at all
#      through rclcpp, BEST_EFFORT, on a clean link
#   2. a RELIABLE pair with 8% loss injected inside TickLE - the only way to make rmw's reorder
#      buffer actually hold something (loopback loses nothing, and the matrix is BEST_EFFORT)
#   3. a BEST_EFFORT pair with the same loss - the discard path through rmw
#
# WHICH LIBRARY, proved rather than assumed. Neither 0b415269 nor d63860c7 added a string literal,
# so a marker string cannot tell the shipping build from the one before it - a known limit of that
# technique. Identity is instead: the mapped librmw_tickle.so is THIS checkout's install AND was
# written after this script's own build started. ~/rmw_perf_ws/install/setup.bash puts the CI
# runner's (now stale, hardware CI being dispatch-only) rmw_tickle ahead of anything sourced later,
# so this checkout is forced to the front of AMENT_PREFIX_PATH and LD_LIBRARY_PATH.
#
# HOW TO READ IT, written before running:
#   any part whose library identity check fails is VOID, whatever its numbers say
#   any part with delivered far below what was published is VOID (the pair never matched)
#   1: all 4 cells pass, 4 benchmark results, timestamp_not_newer=0 and out_of_order=0 per cell
#   2: reorder_held_peak > 0 (the buffer really held), reorder_overflow=0, out_of_order=0,
#      timestamp_not_newer=0. held_peak == 0 under injected loss means rmw's buffer is not wired.
#   3: out_of_order_discarded > 0 (the discard fired) and out_of_order=0
set -o pipefail
REPO=/home/semih/tickle; WS="$HOME/rmw_perf_ws"; DROP="${DROP:-8}"
OUT="${OUT:-/tmp/tickle_rmw_check_$(date +%Y%m%d-%H%M%S)}"; mkdir -p "$OUT"
exec > >(tee -a "$OUT/summary.txt") 2>&1
LIB="$REPO/install/rmw_tickle/lib/librmw_tickle.so"
echo "=== rmw shipping check, repo $(git -C "$REPO" rev-parse --short HEAD), $(date -Is), out $OUT ==="

set +u
# shellcheck disable=SC1091 # ROS setup scripts are generated at install time.
. /opt/ros/lyrical/setup.bash
# shellcheck disable=SC1091
. "$WS/install/setup.bash"
set -u

build() { # $1 = extra CFLAGS
    BUILD_START=$(date +%s)
    rm -rf "$REPO/build/rmw_tickle" "$REPO/install/rmw_tickle"
    (cd "$REPO" && colcon build --packages-select rmw_tickle rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp \
        --cmake-args -DBUILD_SHARED_LIBS=ON "-DCMAKE_C_FLAGS=$1") > "$OUT/build_$(date +%s).log" 2>&1 \
        || { echo "BUILD FAILED (flags: $1)"; exit 1; }
    set +u
    # shellcheck disable=SC1091
    . "$REPO/install/setup.bash"
    set -u
    export AMENT_PREFIX_PATH="$REPO/install/rmw_tickle:$AMENT_PREFIX_PATH"
    export LD_LIBRARY_PATH="$REPO/install/rmw_tickle/lib:${LD_LIBRARY_PATH:-}"
    export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=127.255.255.255
}

identity() { # $1 = pid; prints OK or VOID with the reason
    local mapped mt
    mapped=$(grep -o '/[^ ]*librmw_tickle[^ ]*' "/proc/$1/maps" 2>/dev/null | sort -u | head -1)
    [ -n "$mapped" ] || { echo "VOID: pid $1 has no librmw_tickle.so mapped"; return 1; }
    [ "$(readlink -f "$mapped")" = "$(readlink -f "$LIB")" ] || { echo "VOID: mapped $mapped, not this checkout's $LIB"; return 1; }
    mt=$(stat -c %Y "$mapped")
    [ "$mt" -ge "$BUILD_START" ] || { echo "VOID: $mapped predates this build (mtime $mt < $BUILD_START)"; return 1; }
    echo "OK: mapped $mapped, built $(date -d "@$mt" +%H:%M:%S) after this build began"
}

# ---- part 1: the shipping build through the real benchmark matrix --------------------------------
build ""
echo; echo "--- part 1: buildfarm two_process_rmw_ matrix, shipping build ---"
rm -rf "$WS/build/buildfarm_perf_tests/test_results" "$WS/build/buildfarm_perf_tests/launch_test"
colcon test --base-paths "$WS" --build-base "$WS/build" --install-base "$WS/install" \
    --packages-select buildfarm_perf_tests --return-code-on-test-failure \
    --ctest-args -R two_process_rmw_tickle > "$OUT/part1_colcon.log" 2>&1 &
cpid=$!
id1="VOID: never saw a perf_test with rmw_tickle mapped"
for _ in $(seq 1 120); do
    for p in $(ps -eo pid,cmd --no-headers | awk '/perf_test/ && !/awk/ {print $1}'); do
        grep -q librmw_tickle "/proc/$p/maps" 2>/dev/null && { id1=$(identity "$p"); break 2; }
    done
    sleep 1
done
wait "$cpid"; rc=$?
echo "identity: $id1"
echo "colcon test exit: $rc"
L="$WS/build/buildfarm_perf_tests/launch_test"
echo "benchmark results produced: $(find "$WS/build/buildfarm_perf_tests/test_results" -name '*.benchmark.json' 2>/dev/null | wc -l) of 4"
for f in "$L"/*two_process_rmw_tickle*.txt; do
    [ -f "$f" ] || continue
    printf '  %-26s %s\n' "$(basename "$f" .txt | sed 's/test_performance_two_process_rmw_tickle_//')" \
        "$(grep -aoE 'delivered=[0-9]+ out_of_order=[0-9]+ timestamp_not_newer=[0-9]+' "$f" | tail -1)"
done
echo "patch (f) timestamp violations: $({ grep -rh 'TICKLE-PATCH-F timestamp_not_newer' "$L" 2>/dev/null || true; } | wc -l)"

# ---- parts 2 and 3: RELIABLE and BEST_EFFORT pairs with loss injected inside TickLE ---------------
build "-DTT_RX_DROP_PERCENT=$DROP"
pt="$WS/install/performance_test/lib/performance_test/perf_test"
pair() { # $1 label, $2 reliable|besteffort
    local common=(-c ROS2 -t Array1k --max_runtime 15 --rate 0 --keep_last --history_depth 10)
    [ "$2" = reliable ] && common+=(--reliable)
    local rosargs=(--ros-args --param start_type_description_service:=false)
    TICKLE_NODE_ID=101 "$pt" "${common[@]}" -p 1 -s 0 "${rosargs[@]}" > "$OUT/$1_pub.log" 2>&1 & local pub=$!
    TICKLE_NODE_ID=102 "$pt" "${common[@]}" -p 0 -s 1 "${rosargs[@]}" > "$OUT/$1_sub.log" 2>&1 & local sub=$!
    sleep 2
    local id; id=$(identity "$sub")
    wait "$pub" 2>/dev/null; wait "$sub" 2>/dev/null
    echo; echo "--- $1 ($2, ${DROP}% loss injected in TickLE's receive path) ---"
    echo "identity: $id"
    grep -a 'delivery:' "$OUT/$1_sub.log" | tail -1 | tr ' ' '\n' | grep '=' | paste -sd' '
}
pair part2 reliable
pair part3 besteffort

build ""   # leave this checkout on the shipping build, not the loss-injected one
echo; echo "=== done $(date -Is); this checkout restored to the shipping build ==="
