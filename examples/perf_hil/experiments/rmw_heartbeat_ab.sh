#!/usr/bin/env bash
# Does a periodic RELIABLE heartbeat stop rmw_tickle's window jumps under the default ROS 2 profile?
#
# Background (2026-09-24): rmw_shipping_check.sh part 2 - RELIABLE, KEEP_LAST depth 10, max rate,
# 8% loss injected in TickLE's receive path - showed ~1100 "Reliable gap too large to track" window
# jumps in 15s. The reading being tested: TickLE already has DDS's GAP (the eviction Heartbeat that
# answers an ACKNACK naming an evicted sample), but not its backstop. When that ACKNACK or its reply
# is lost, only acknack_retry (1ms x tt_RELIABLE_RETRY) is left, and a max-rate stream fills the
# 1024-slot window in ~5ms. RTPS's backstop is a HEARTBEAT carrying firstSN that does not depend on
# one exchange; TickLE has one (send_heartbeat()), off by default and never armed by rmw_tickle
# until b1c00e7a added RMW_TICKLE_HEARTBEAT_PERIOD_NS.
#
# Two arms, same build, alternated within each rep (order flipped every rep) so drift lands on both:
#   off: RMW_TICKLE_HEARTBEAT_PERIOD_NS unset  (the control; should reproduce ~1100 jumps)
#   on:  RMW_TICKLE_HEARTBEAT_PERIOD_NS=$HB_NS (default 1ms)
#
# HOW TO READ IT, written before running:
#   VOID per run: library identity fails; the on arm's publisher did not log "periodic heartbeat
#     armed" or the off arm's did; delivered far below the other arm's.
#   VOID overall: the off arm does not reproduce hundreds of jumps (the control failed).
#   Must hold in every run whatever else happens: out_of_order=0, timestamp_not_newer=0,
#     reorder_overflow=0 (e300fe72's fix).
#   jumps, per arm, over REPS runs:
#     every on < every off AND on median <= half the off median -> the missing backstop is the
#       mechanism; the fix is a first_available signal that survives one lost exchange
#     every on < every off but by less than half -> the backstop helps the tail where consecutive
#       exchanges are lost; it is part of the mechanism, not all of it (Dev's caveat: a 1ms
#       heartbeat is a second channel at roughly the retry's own period)
#     ranges overlap -> no effect this experiment can resolve; the reading is wrong or too weak
#   out_of_order_discarded is reported but not judged: it cannot tell a duplicate retransmission
#   from late new data (both are seq <= highest_delivered), so it is not a loss measure.
set -o pipefail
REPO=/home/semih/tickle; WS="$HOME/rmw_perf_ws"; DROP="${DROP:-8}"; REPS="${REPS:-5}"; HB_NS="${HB_NS:-1000000}"
OUT="${OUT:-/tmp/tickle_rmw_hb_ab_$(date +%Y%m%d-%H%M%S)}"; mkdir -p "$OUT"
LIB="$REPO/install/rmw_tickle/lib/librmw_tickle.so"

# Box scope: both processes run on this PC; never the rpi lock for a box-only job.
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then
    OUT="$OUT" RIG_LOCK_SCOPE=box exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
exec > >(tee -a "$OUT/summary.txt") 2>&1
echo "=== rmw heartbeat A/B, repo $(git -C "$REPO" rev-parse --short HEAD), $(date -Is), ${REPS} reps, HB_NS=$HB_NS, ${DROP}% loss, out $OUT ==="

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
    [ -n "$mapped" ] || { echo "VOID(no librmw_tickle mapped in $1)"; return 1; }
    [ "$(readlink -f "$mapped")" = "$(readlink -f "$LIB")" ] || { echo "VOID(mapped $mapped)"; return 1; }
    mt=$(stat -c %Y "$mapped")
    [ "$mt" -ge "$BUILD_START" ] || { echo "VOID(predates build)"; return 1; }
    echo OK
}

build "-DTT_RX_DROP_PERCENT=$DROP"
pt="$WS/install/performance_test/lib/performance_test/perf_test"
run() { # $1 arm (off|on), $2 rep
    local tag="$1_rep$2" common=(-c ROS2 -t Array1k --max_runtime 15 --rate 0 --keep_last --history_depth 10 --reliable)
    local rosargs=(--ros-args --param start_type_description_service:=false) hb=()
    [ "$1" = on ] && hb=(RMW_TICKLE_HEARTBEAT_PERIOD_NS="$HB_NS")
    env "${hb[@]}" TICKLE_NODE_ID=101 "$pt" "${common[@]}" -p 1 -s 0 "${rosargs[@]}" > "$OUT/${tag}_pub.log" 2>&1 & local pub=$!
    env "${hb[@]}" TICKLE_NODE_ID=102 "$pt" "${common[@]}" -p 0 -s 1 "${rosargs[@]}" > "$OUT/${tag}_sub.log" 2>&1 & local sub=$!
    sleep 2
    local id; id=$(identity "$sub")
    wait "$pub" 2>/dev/null; wait "$sub" 2>/dev/null
    local armed=no; grep -aq 'periodic heartbeat armed' "$OUT/${tag}_pub.log" && armed=yes
    local notarmed=""; grep -aq 'requested but not armed' "$OUT/${tag}_pub.log" && notarmed=" ARM-FAILED"
    local want=no; [ "$1" = on ] && want=yes
    local arm_ok=OK; [ "$armed" = "$want" ] && [ -z "$notarmed" ] || arm_ok="VOID(armed=$armed$notarmed)"
    local c; c=$(grep -a 'delivery:' "$OUT/${tag}_sub.log" | tail -1 | tr ' ' '\n' | grep '=' | paste -sd' ')
    printf '%-3s rep %s: identity=%s arm=%s jumps=%s %s\n' "$1" "$2" "$id" "$arm_ok" \
        "$(grep -ac 'gap too large' "$OUT/${tag}_sub.log")" "$c"
}
for rep in $(seq 1 "$REPS"); do
    if [ $((rep % 2)) = 1 ]; then run off "$rep"; run on "$rep"; else run on "$rep"; run off "$rep"; fi
done

build ""   # leave this checkout on the shipping build, not the loss-injected one
echo "=== done $(date -Is); this checkout restored to the shipping build ==="
