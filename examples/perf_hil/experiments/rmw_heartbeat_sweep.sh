#!/usr/bin/env bash
# Piggybacked vs periodic RELIABLE heartbeat through rmw_tickle, under the default ROS 2 profile.
#
# Follows rmw_heartbeat_ab.sh (9afacfe1), which showed that arming the existing periodic Heartbeat
# at 1ms cut window jumps from a median 1035 to 5 per 15s run at max rate with 8% loss - so the
# missing piece was a first_available signal that survives one lost ACKNACK/reply. A 1ms timer is
# the wrong carrier for a default, though: it costs a datagram per millisecond per publisher whatever
# the data rate, i.e. most of the traffic on a quiet publisher. The user chose to try piggybacking
# the Heartbeat on DATA every N samples instead (RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY), whose
# frequency scales with the data rate.
#
# Phase A - max rate, where the window fills in ~5ms and the jumps happen:
#   off | pb16 | pb64 | pb256 | hb1ms (the periodic reference), REPS runs each, arm order rotated
#   every rep so drift lands on all arms.
# Phase B - 1000 msg/s, the quiet-publisher regime. Piggyback every 64 then fires only every 64ms,
#   so it has a hole there that periodic does not (Dev's point: piggyback only speaks while data
#   flows). perf_test cannot stop a stream mid-run, so this is the measurable form of the
#   burst-then-stop case: off | pb64 | hb1ms, REPS_B runs each.
#
# HOW TO READ IT, written before running:
#   VOID per run: library identity fails; the arm's "armed" log line is missing, or another
#     arm's is present; delivered far below the same phase's other arms.
#   VOID phase A: the off arm does not reproduce hundreds of jumps (the control failed).
#   Must hold in every run: out_of_order=0, timestamp_not_newer=0, reorder_overflow=0.
#   Phase A, jumps per arm against the hb1ms reference:
#     a pbN arm whose runs all fall inside hb1ms's range (last time 0-6) -> equivalent fix
#     every pbN run below every off run but above the hb1ms range -> partial fix at that N
#     ranges overlapping the off arm -> no effect at that N
#   Phase B, jumps are expected to be ~0 in every arm (a 1024 window takes ~1s to fill at this
#   rate, far longer than the retry give-up). The metric is the latency tail, the worst per-second
#   latency_max over the run:
#     pb64 ~ off and hb1ms clearly below both -> the hole is real and costs latency on a quiet
#       publisher; the default shape needs a slow periodic heartbeat alongside piggyback
#     all three overlapping -> the hole exists but the retry covers it here; piggyback alone is
#       enough for this regime
#     pb64 clearly worse than off -> unexpected; investigate before reading anything else
#   out_of_order_discarded is reported but not judged: it cannot tell a duplicate retransmission
#   from late new data (both are seq <= highest_delivered).
#   This is a behaviour check on one PC. The wire cost is measured separately on the rig.
set -o pipefail
REPO=/home/semih/tickle; WS="$HOME/rmw_perf_ws"; DROP="${DROP:-8}"
REPS="${REPS:-5}"; REPS_B="${REPS_B:-3}"; RATE_B="${RATE_B:-1000}"
OUT="${OUT:-/tmp/tickle_rmw_hb_sweep_$(date +%Y%m%d-%H%M%S)}"; mkdir -p "$OUT"
LIB="$REPO/install/rmw_tickle/lib/librmw_tickle.so"

# Box scope: both processes run on this PC; never the rpi lock for a box-only job.
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then
    OUT="$OUT" RIG_LOCK_SCOPE=box exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
exec > >(tee -a "$OUT/summary.txt") 2>&1
echo "=== rmw heartbeat sweep, repo $(git -C "$REPO" rev-parse --short HEAD), $(date -Is), ${DROP}% loss, A: ${REPS} reps max rate, B: ${REPS_B} reps at ${RATE_B}/s, out $OUT ==="

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

# Arm label -> environment. The "armed" check below derives what each arm must and must not log.
arm_env() {
    case "$1" in
        off)   ;;
        hb1ms) echo "RMW_TICKLE_HEARTBEAT_PERIOD_NS=1000000" ;;
        pb*)   echo "RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY=${1#pb}" ;;
    esac
}
arm_check() { # $1 arm, $2 pub log; prints OK or VOID(...)
    local periodic=no piggy="" bad=""
    grep -aq 'periodic heartbeat armed' "$2" && periodic=yes
    piggy=$(grep -aoE 'heartbeat piggyback armed: every [0-9]+' "$2" | grep -oE '[0-9]+$' | head -1)
    grep -aqE 'requested but not armed' "$2" && bad=" ARM-FAILED"
    case "$1" in
        off)   [ "$periodic" = no ] && [ -z "$piggy" ] ;;
        hb1ms) [ "$periodic" = yes ] && [ -z "$piggy" ] ;;
        pb*)   [ "$periodic" = no ] && [ "$piggy" = "${1#pb}" ] ;;
    esac && [ -z "$bad" ] && { echo OK; return; }
    echo "VOID(periodic=$periodic piggyback=${piggy:-none}$bad)"
}

pt="$WS/install/performance_test/lib/performance_test/perf_test"
run() { # $1 phase, $2 arm, $3 rep, $4 rate (0 = max)
    local tag="$1_$2_rep$3" common=(-c ROS2 -t Array1k --max_runtime 15 --rate "$4" --keep_last --history_depth 10 --reliable)
    local rosargs=(--ros-args --param start_type_description_service:=false) env_arm=()
    local e; e=$(arm_env "$2"); [ -n "$e" ] && env_arm=("$e")
    env "${env_arm[@]}" TICKLE_NODE_ID=101 "$pt" "${common[@]}" -p 1 -s 0 "${rosargs[@]}" > "$OUT/${tag}_pub.log" 2>&1 & local pub=$!
    env "${env_arm[@]}" TICKLE_NODE_ID=102 "$pt" "${common[@]}" -p 0 -s 1 "${rosargs[@]}" > "$OUT/${tag}_sub.log" 2>&1 & local sub=$!
    sleep 2
    local id; id=$(identity "$sub")
    wait "$pub" 2>/dev/null; wait "$sub" 2>/dev/null
    local c; c=$(grep -a 'delivery:' "$OUT/${tag}_sub.log" | tail -1 | tr ' ' '\n' | grep '=' | paste -sd' ')
    # perf_test's per-second rows: col 9 latency_max, col 10 latency_mean (ms). First second skipped
    # (discovery). Worst max and average mean over the rest.
    local lat; lat=$(grep -aE '^[0-9]+\.[0-9]+,' "$OUT/${tag}_sub.log" | tail -n +2 | tr -d '\t' |
        awk -F, '{ if ($9 > mx) mx = $9; s += $10; n++ } END { if (n) printf "lat_max_ms=%.3f lat_mean_ms=%.4f", mx, s / n; else printf "lat=none" }')
    printf '%s %-5s rep %s: identity=%s arm=%s jumps=%s %s %s\n' "$1" "$2" "$3" "$id" "$(arm_check "$2" "$OUT/${tag}_pub.log")" \
        "$(grep -ac 'gap too large' "$OUT/${tag}_sub.log")" "$lat" "$c"
}
rotate() { # $1 rep, rest = arms; prints the arms rotated left by (rep - 1)
    local r=$1; shift; local a=("$@") n=$# i
    for ((i = 0; i < n; i++)); do printf '%s ' "${a[(i + r - 1) % n]}"; done
}

build "-DTT_RX_DROP_PERCENT=$DROP"
echo "--- phase A: max rate ---"
for rep in $(seq 1 "$REPS"); do
    for arm in $(rotate "$rep" off pb16 pb64 pb256 hb1ms); do run A "$arm" "$rep" 0; done
done
echo "--- phase B: ${RATE_B} msg/s ---"
for rep in $(seq 1 "$REPS_B"); do
    for arm in $(rotate "$rep" off pb64 hb1ms); do run B "$arm" "$rep" "$RATE_B"; done
done

build ""   # leave this checkout on the shipping build, not the loss-injected one
echo "=== done $(date -Is); this checkout restored to the shipping build ==="
