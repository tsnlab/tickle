#!/usr/bin/env bash
# Is the doubling of out_of_order_discarded at fddad207 caused by N = 65507, or by the socket
# buffer request that commit raised from 1 MiB to 4 MiB?
#
# On this box net.core.rmem_max/wmem_max = 4 MiB, so the 4 MiB request is granted in full, and the
# kernel doubles it to ~8 MiB per socket where it used to be ~2 MiB (TickLE Dev's reading). Deeper
# queues on both the data socket and the well-known socket could let more samples arrive late
# through the other socket rather than be dropped - counted as discarded instead of never arriving.
#
# Two arms, both at the new default N = 65507, both with 8% injected receive loss. They are
# interleaved per rep, with a rebuild on each switch; every build is proved by library identity:
#   default  - fddad207 as shipped (4 MiB request)
#   sock1m   - tt_SOCKET_BUFFER_SIZE=1048576 (core's previous request). It is set by patching
#              rmw_tickle/CMakeLists.txt's target_compile_definitions for the build and restoring it
#              right after, because a -D in CMAKE_C_FLAGS lands before that definition on the
#              command line and loses. Each build then proves the value in the compile flags.
# RELIABLE and BEST_EFFORT pairs each, perf_test Array1k, max rate, 15 s, KEEP_LAST 10.
#
# HOW TO READ IT, written before running:
#   - sock1m discards all back near ~5000 (part-3 level at N = 1472) and default all near ~10000
#     -> the buffer, not N.
#   - Both arms alike -> not the buffer. Then N or noise is the suspect, and the 1472 build is the
#     next arm.
#   - Must hold in every run: out_of_order = 0 and timestamp_not_newer = 0, plus reorder_overflow
#     = 0 for RELIABLE.
#   - A behaviour check on the dev box, not a performance figure.
set -o pipefail
REPO=/home/semih/tickle; WS="$HOME/rmw_perf_ws"; DROP="${DROP:-8}"; REPS="${REPS:-3}"
OUT="${OUT:-/tmp/tickle_rmw_sockbuf_ab_$(date +%Y%m%d-%H%M%S)}"; mkdir -p "$OUT"
LIB="$REPO/install/rmw_tickle/lib/librmw_tickle.so"
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then
    OUT="$OUT" RIG_LOCK_SCOPE=box exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
exec > >(tee -a "$OUT/summary.txt") 2>&1
echo "=== rmw socket-buffer A/B, repo $(git -C "$REPO" rev-parse --short HEAD), $(date -Is), ${REPS} reps, ${DROP}% loss, out $OUT ==="
set +u
# shellcheck disable=SC1091
. /opt/ros/lyrical/setup.bash
# shellcheck disable=SC1091
. "$WS/install/setup.bash"
set -u

CML="$REPO/rmw_tickle/rmw_tickle/CMakeLists.txt"
trap 'git -C "$REPO" checkout -- rmw_tickle/rmw_tickle/CMakeLists.txt' EXIT
build() { # $1 = extra CFLAGS, $2 = socket buffer bytes to compile in
    BUILD_START=$(date +%s)
    git -C "$REPO" checkout -- rmw_tickle/rmw_tickle/CMakeLists.txt
    sed -i "s/tt_SOCKET_BUFFER_SIZE=4194304/tt_SOCKET_BUFFER_SIZE=$2/" "$CML"
    grep -q "tt_SOCKET_BUFFER_SIZE=$2)" "$CML" || { echo "PATCH FAILED"; exit 1; }
    rm -rf "$REPO/build/rmw_tickle" "$REPO/install/rmw_tickle"
    (cd "$REPO" && colcon build --packages-select rmw_tickle rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp \
        --cmake-args -DBUILD_SHARED_LIBS=ON "-DCMAKE_C_FLAGS=$1") > "$OUT/build_$(date +%s).log" 2>&1 \
        || { echo "BUILD FAILED (flags: $1)"; exit 1; }
    git -C "$REPO" checkout -- rmw_tickle/rmw_tickle/CMakeLists.txt
    grep -rqs "tt_SOCKET_BUFFER_SIZE=$2" "$REPO/build/rmw_tickle/CMakeFiles/rmw_tickle.dir/flags.make" \
        || { echo "VOID: tt_SOCKET_BUFFER_SIZE=$2 is not in rmw_tickle's compile flags"; exit 1; }
    echo "built with tt_SOCKET_BUFFER_SIZE=$2 (proved in flags.make)"
    set +u
    # shellcheck disable=SC1091
    . "$REPO/install/setup.bash"
    set -u
    export AMENT_PREFIX_PATH="$REPO/install/rmw_tickle:$AMENT_PREFIX_PATH"
    export LD_LIBRARY_PATH="$REPO/install/rmw_tickle/lib:${LD_LIBRARY_PATH:-}"
    export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=127.255.255.255
}
identity() {
    local mapped mt
    mapped=$(grep -o '/[^ ]*librmw_tickle[^ ]*' "/proc/$1/maps" 2>/dev/null | sort -u | head -1)
    [ -n "$mapped" ] || { echo "VOID(no librmw_tickle mapped)"; return 1; }
    [ "$(readlink -f "$mapped")" = "$(readlink -f "$LIB")" ] || { echo "VOID(mapped $mapped)"; return 1; }
    mt=$(stat -c %Y "$mapped")
    [ "$mt" -ge "$BUILD_START" ] || { echo "VOID(predates build)"; return 1; }
    echo OK
}
pt="$WS/install/performance_test/lib/performance_test/perf_test"
pair() { # $1 tag, $2 reliable|besteffort
    local common=(-c ROS2 -t Array1k --max_runtime 15 --rate 0 --keep_last --history_depth 10)
    [ "$2" = reliable ] && common+=(--reliable)
    local rosargs=(--ros-args --param start_type_description_service:=false)
    TICKLE_NODE_ID=101 "$pt" "${common[@]}" -p 1 -s 0 "${rosargs[@]}" > "$OUT/$1_pub.log" 2>&1 & local pub=$!
    TICKLE_NODE_ID=102 "$pt" "${common[@]}" -p 0 -s 1 "${rosargs[@]}" > "$OUT/$1_sub.log" 2>&1 & local sub=$!
    sleep 2
    local id; id=$(identity "$sub")
    wait "$pub" 2>/dev/null; wait "$sub" 2>/dev/null
    local rcv; rcv=$(grep -aoE 'receive buffer[^,]*|SO_RCVBUF[^,]*' "$OUT/$1_sub.log" | head -1)
    printf '%s: identity=%s jumps=%s %s | %s\n' "$1" "$id" "$(grep -ac 'gap too large' "$OUT/$1_sub.log")" \
        "$(grep -a 'delivery:' "$OUT/$1_sub.log" | tail -1 | tr ' ' '\n' | grep -E '^(delivered|out_of_order|timestamp_not_newer|out_of_order_discarded|reorder_overflow)=' | paste -sd' ')" "${rcv:-no sockbuf log line}"
}
for rep in $(seq 1 "$REPS"); do
    if [ $((rep % 2)) = 1 ]; then order="default sock1m"; else order="sock1m default"; fi
    for arm in $order; do
        if [ "$arm" = default ]; then build "-DTT_RX_DROP_PERCENT=$DROP" 4194304; else build "-DTT_RX_DROP_PERCENT=$DROP" 1048576; fi
        pair "${arm}_rel_rep$rep" reliable
        pair "${arm}_be_rep$rep" besteffort
    done
done
build "" 4194304
echo "=== done $(date -Is); this checkout restored to the shipping build ==="
