#!/usr/bin/env bash
# What a RELIABLE Heartbeat costs on the wire through rmw_tickle, counted from a loopback capture.
#
# rmw_heartbeat_sweep.sh (b97eb126) showed that piggybacking a Heartbeat on every 64th DATA stops
# the window jumps as well as a 1ms periodic Heartbeat does. This counts what each one costs, rather
# than timing it: the analytic piggyback cost is ~0.05% of a 1 KB stream, far below what a
# throughput A/B could resolve, and rmw performance on the rig waits for the rig upgrade (user,
# 2026-09-24). Counting is exact, so a PC is fine for it.
#
# Shipping build, NO injected loss: any ACKNACK or retransmitted DATA here was not needed by a lost
# sample. Dev's concern, 2026-09-24: a periodic Heartbeat is its own datagram, so it can overtake
# the DATA it covers (e.g. read from a different socket), making the reader NACK a sample that was
# never lost and the publisher retransmit a duplicate. Piggyback cannot, since its Heartbeat follows
# the DATA in the same datagram.
#
# Arms off | pb64 | hb1ms, at 1000 msg/s (10s) and at max rate (3s, capture size), REPS each,
# rotated. Publisher is node 101, subscriber node 102.
#
# HOW TO READ IT, written before running:
#   VOID per run: library identity or arm check fails; tcpdump reports any packet dropped by the
#     kernel; malformed > 0.
#   Controls, which must hold or the counting is not measuring what it claims:
#     off:   hb_piggyback = 0
#     pb64:  hb_piggyback within 2% of data/64, and publisher datagrams per sample equal to off's
#            within 1% (piggyback adds no datagram)
#     hb1ms: hb_alone between 0.7x and 1.0x of 1000 x pub_lifetime_s (the timer starts only once
#            the first sample is published, after discovery, so a little under the lifetime)
#   Cost, per arm: publisher udp_payload_bytes per DATA and datagrams per DATA, against off.
#   Spurious traffic: subscriber acknack and publisher data_retx per 1000 DATA. hb1ms clearly above
#     off in both rates -> Dev's overtaking mechanism is real and costs traffic; ~equal -> not seen.
set -o pipefail
REPO=/home/semih/tickle; WS="$HOME/rmw_perf_ws"; REPS="${REPS:-2}"
HERE="$REPO/examples/perf_hil/experiments"
OUT="${OUT:-/tmp/tickle_rmw_hb_wire_$(date +%Y%m%d-%H%M%S)}"; mkdir -p "$OUT"
LIB="$REPO/install/rmw_tickle/lib/librmw_tickle.so"

if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then
    OUT="$OUT" RIG_LOCK_SCOPE=box exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
exec > >(tee -a "$OUT/summary.txt") 2>&1
echo "=== rmw heartbeat wire cost, repo $(git -C "$REPO" rev-parse --short HEAD), $(date -Is), ${REPS} reps, out $OUT ==="

set +u
# shellcheck disable=SC1091 # ROS setup scripts are generated at install time.
. /opt/ros/lyrical/setup.bash
# shellcheck disable=SC1091
. "$WS/install/setup.bash"
set -u

BUILD_START=$(date +%s)
rm -rf "$REPO/build/rmw_tickle" "$REPO/install/rmw_tickle"
(cd "$REPO" && colcon build --packages-select rmw_tickle rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp \
    --cmake-args -DBUILD_SHARED_LIBS=ON "-DCMAKE_C_FLAGS=") > "$OUT/build.log" 2>&1 || { echo "BUILD FAILED"; exit 1; }
set +u
# shellcheck disable=SC1091
. "$REPO/install/setup.bash"
set -u
export AMENT_PREFIX_PATH="$REPO/install/rmw_tickle:$AMENT_PREFIX_PATH"
export LD_LIBRARY_PATH="$REPO/install/rmw_tickle/lib:${LD_LIBRARY_PATH:-}"
export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=127.255.255.255

identity() {
    local mapped mt
    mapped=$(grep -o '/[^ ]*librmw_tickle[^ ]*' "/proc/$1/maps" 2>/dev/null | sort -u | head -1)
    [ -n "$mapped" ] || { echo "VOID(no librmw_tickle mapped in $1)"; return 1; }
    [ "$(readlink -f "$mapped")" = "$(readlink -f "$LIB")" ] || { echo "VOID(mapped $mapped)"; return 1; }
    mt=$(stat -c %Y "$mapped")
    [ "$mt" -ge "$BUILD_START" ] || { echo "VOID(predates build)"; return 1; }
    echo OK
}
arm_env() {
    case "$1" in
        # Piggyback is on by default since the user's decision of 2026-09-24, so "off" and the
        # periodic-only reference must switch it off explicitly rather than by leaving it unset.
        off)   echo "RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY=0" ;;
        hb1ms) echo "RMW_TICKLE_HEARTBEAT_PERIOD_NS=1000000 RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY=0" ;;
        pb*)   echo "RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY=${1#pb}" ;;
    esac
}
arm_check() {
    local periodic=no piggy="" bad=""
    grep -aq 'periodic heartbeat armed' "$2" && periodic=yes
    piggy=$(grep -aoE 'heartbeat piggyback armed: every [0-9]+' "$2" | grep -oE '[0-9]+$' | head -1)
    # d6d312cd: piggyback is on (64) by default and an explicit 0 logs this line, so "off" is
    # positively confirmed rather than inferred from a missing armed line.
    grep -aq 'heartbeat piggyback off' "$2" && piggy="${piggy:-off}"
    grep -aqE 'requested but not armed' "$2" && bad=" ARM-FAILED"
    case "$1" in
        off)   [ "$periodic" = no ] && [ "$piggy" = off ] ;;
        hb1ms) [ "$periodic" = yes ] && [ "$piggy" = off ] ;;
        pb*)   [ "$periodic" = no ] && [ "$piggy" = "${1#pb}" ] ;;
    esac && [ -z "$bad" ] && { echo OK; return; }
    echo "VOID(periodic=$periodic piggyback=${piggy:-none}$bad)"
}
rotate() {
    local r=$1; shift; local a=("$@") n=$# i
    for ((i = 0; i < n; i++)); do printf '%s ' "${a[(i + r - 1) % n]}"; done
}

pt="$WS/install/performance_test/lib/performance_test/perf_test"
run() { # $1 arm, $2 rep, $3 rate, $4 seconds
    local tag="r$3_$1_rep$2" pcap="$OUT/r$3_$1_rep$2.pcap"
    local common=(-c ROS2 -t Array1k --max_runtime "$4" --rate "$3" --keep_last --history_depth 10 --reliable)
    local rosargs=(--ros-args --param start_type_description_service:=false) env_arm=()
    local e; e=$(arm_env "$1"); [ -n "$e" ] && read -ra env_arm <<<"$e"
    # PID from the launch itself (CLAUDE.md rule 3), so the stop below can only hit this tcpdump.
    tcpdump -i lo -s 0 -B 262144 -U -w "$pcap" udp > "$OUT/${tag}_tcpdump.log" 2>&1 & local td=$!
    for _ in $(seq 1 50); do grep -q 'listening on' "$OUT/${tag}_tcpdump.log" && break; sleep 0.1; done
    local t0; t0=$(date +%s.%N)
    env "${env_arm[@]}" TICKLE_NODE_ID=101 "$pt" "${common[@]}" -p 1 -s 0 "${rosargs[@]}" > "$OUT/${tag}_pub.log" 2>&1 & local pub=$!
    env "${env_arm[@]}" TICKLE_NODE_ID=102 "$pt" "${common[@]}" -p 0 -s 1 "${rosargs[@]}" > "$OUT/${tag}_sub.log" 2>&1 & local sub=$!
    sleep 2
    local id; id=$(identity "$sub")
    wait "$pub" 2>/dev/null; local pub_s; pub_s=$(echo "$(date +%s.%N) - $t0" | bc)
    wait "$sub" 2>/dev/null
    kill -INT "$td"; wait "$td" 2>/dev/null
    local dropped; dropped=$(grep -oE '[0-9]+ packets dropped by kernel' "$OUT/${tag}_tcpdump.log" | grep -oE '^[0-9]+')
    echo "$tag: identity=$id arm=$(arm_check "$1" "$OUT/${tag}_pub.log") kernel_dropped=${dropped:-UNKNOWN} pub_lifetime_s=$pub_s"
    python3 "$HERE/tickle_pcap_count.py" "$pcap" | grep -E '^node10[12] ' | sed 's/^/    /'
    rm -f "$pcap" # up to ~1 GB at max rate; the counts above are the result
}

echo "--- 1000 msg/s, 10s ---"
for rep in $(seq 1 "$REPS"); do for arm in $(rotate "$rep" off pb64 hb1ms); do run "$arm" "$rep" 1000 10; done; done
echo "--- max rate, 3s ---"
for rep in $(seq 1 "$REPS"); do for arm in $(rotate "$rep" off pb64 hb1ms); do run "$arm" "$rep" 0 3; done; done
echo "=== done $(date -Is) ==="
