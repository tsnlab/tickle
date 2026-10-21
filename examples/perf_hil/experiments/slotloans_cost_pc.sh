#!/usr/bin/env bash
# slotloans_cost_pc.sh - what does a slot-loan publish (RMW_TICKLE_LOAN_PUBLISH_SLOTS=1) cost per sample, arm against
# arm, on the PC? Written 2026-10-21 for the rig's ab_loans slots verdict L WORSE (A=04c71310, B=f588b101: rtt
# array1k block +2.6 us best_effort, +1.85 us reliable; tput Array4k best_effort loans -13.7% delivered/s).
#
# Arms are rmw installs (ARMS="A=<install> B=<install> ..."), each holding rmw_tickle and rmw_perf_pingpong; perf_test
# is PERF_WS's (its typesupport is the same in every arm). Blocks: the arms in order, then reversed, REPS times
# (A B C C B A ...). Each run in a fresh private netns with a private /dev/shm, every process pinned (CPU_C, CPU_S)
# and under `perf stat -e instructions:u,instructions:k,cycles:u,cycles:k`.
# Cells (CELLS, default all):
#   rtt_be    pong_node + ping_node array1k best_effort block, -i $RTT_I (default 0: back to back, so the round
#             trip, not the idle wait between pings, is what is counted) -d $DUR, loans on both sides
#   rtt_rel   the same, --reliable
#   tput_l    perf_test Array4k best_effort KEEP_LAST 1 -r 0, loans arm (ROS_DISABLE_LOANED_MESSAGES=0 ...)
#   tput_d    the same, shipped defaults (no loan variables): the CONTROL - no arm's change reaches it
# Per sample: rtt - each process's instructions / round trips (ping's RESULT sent); tput - pub / sent (perf_test's
# rows), sub / datagrams it took from its ring (rx_shm on its traffic line). Delivered samples move with how many a
# best-effort reader abandons half-reassembled, far more than any code change does on a shared PC (2026-10-21, first
# run: 38-69k instructions per delivered sample within one arm), so the sub is costed per datagram of input; a sub
# without its traffic line (it did not shut down cleanly) is VOID.
#
# PRE-REGISTERED READING (the python at the end decides, nothing else):
#   VOID  a run without perf counts or without its count of samples; an rtt run of the loans cells whose publishers did
#         not publish in the slot (loans_in_slot 0) - the treatment absent; fewer than 2 runs in an arm; the control
#         tput_d differing between the first arm and another beyond the t threshold (a PC too loaded to read).
#   metric instructions (u + k) per sample per cell and role; cycles printed beside, never deciding (a shared PC).
#   For every arm X against the first arm: WORSE if X - first > 0 beyond the two-sided Welch t threshold at alpha 0.05
#   Bonferroni over the metrics, BETTER the other way, else SAME. Sample counts are printed before any value.
#
# Usage: ARMS="A=/path/install B=/path/install" [REPS=3] [DUR=5] [CELLS="rtt_be rtt_rel tput_l tput_d"] [OUT=prefix]
#        setsid nohup slotloans_cost_pc.sh > log 2>&1 < /dev/null &
#   Needs `sudo -n ip`. The summary ends with "=== slotloans_cost_pc ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARMS=${ARMS:?ARMS=\"A=<install> B=<install>\"}
REPS=${REPS:-3}
DUR=${DUR:-5}
RTT_I=${RTT_I:-0}
CELLS=${CELLS:-"rtt_be rtt_rel tput_l tput_d"}
CPU_C=${CPU_C:-12}
CPU_S=${CPU_S:-13}
PERF_WS=${PERF_WS:-$HOME/rmw_slotloans_A/perf/install}
IFACES=${IFACES:-$HOME/rmw_loans_ws/ifaces/install}
OUT=${OUT:-$HOME/rig_results_safe/slotloans_cost_pc_$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
NS=""
# shellcheck disable=SC2329 # invoked by the EXIT trap below
on_exit() {
    local rc=$?
    [ -n "$NS" ] && sudo -n ip netns del "$NS" 2>/dev/null
    say "=== slotloans_cost_pc ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
command -v perf >/dev/null || { say "FATAL: no perf"; exit 1; }
say "=== slotloans_cost_pc $(date -Is): ARMS='$ARMS' REPS=$REPS DUR=$DUR CELLS='$CELLS' load $(cut -d' ' -f1-3 /proc/loadavg) ==="
read -r -a arm_list <<<"$ARMS"
labels=()
for a in "${arm_list[@]}"; do
    l=${a%%=*} d=${a#*=}
    [ -f "$d/rmw_tickle/lib/librmw_tickle.so" ] && [ -x "$d/rmw_perf_pingpong/lib/rmw_perf_pingpong/ping_node" ] ||
        { say "FATAL: arm $l: no rmw_tickle or ping_node under $d"; exit 1; }
    say "arm $l: $d librmw_tickle md5 $(md5sum <"$d/rmw_tickle/lib/librmw_tickle.so" | cut -c1-12)"
    labels+=("$l")
done
EVENTS="instructions:u,instructions:k,cycles:u,cycles:k"

# The inner run, root inside the namespace (sudo permits only ip): a private /dev/shm, the cell, files handed back.
# shellcheck disable=SC2016 # expanded by the inner bash, inside the namespace
INNER='mount -t tmpfs -o size=512m tmpfs /dev/shm || exit 1
set +u
source /opt/ros/lyrical/setup.bash
source "$IFACES/local_setup.bash"
source "$PERF_WS/local_setup.bash"
source "$INSTALL/local_setup.bash"
set -u
export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=127.255.255.255 ROS_DOMAIN_ID=83 ROS_LOG_DIR="$RUN/roslog"
case "$CELL" in
rtt_*|tput_l) export RMW_TICKLE_LOAN_PUBLISH_SLOTS=1 PINGPONG_LOANED_PUBLISH=1 ROS_DISABLE_LOANED_MESSAGES=0 ;;
esac
cd "$RUN" || exit 1
ps_() { perf stat -x, -e "$EVENTS" -o "$RUN/$1.perf" -- taskset -c "$2" "${@:3}"; }
# The process under perf whose /proc/PID/exe ends in /$2, found among the descendants of $1 (never by name pattern).
node_of() {
    local q c e
    for q in $(cat /proc/"$1"/task/*/children 2>/dev/null); do
        e=$(readlink /proc/"$q"/exe 2>/dev/null)
        case "$e" in */"$2") echo "$q"; return 0 ;; esac
        c=$(node_of "$q" "$2") && { echo "$c"; return 0; }
    done
    return 1
}
case "$CELL" in
rtt_*)
    flag=""; [ "$CELL" = rtt_rel ] && flag="--reliable"
    PP=$INSTALL/rmw_perf_pingpong/lib/rmw_perf_pingpong
    ps_ pong "$CPU_S" "$PP/pong_node" $flag -m array1k >"$RUN/pong.log" 2>&1 &
    pong=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do pong_pid=$(node_of $pong pong_node) && break; sleep 0.5; done
    sleep 2
    pong_pid=$(node_of $pong pong_node)
    grep -o "[^ ]*librmw_tickle.so" /proc/$pong_pid/maps | sort -u >"$RUN/pong.maps"
    ps_ ping "$CPU_C" timeout -s INT -k 5 $((DUR + 30)) "$PP/ping_node" -i "$RTT_I" -d "$DUR" $flag --wait block -m array1k >"$RUN/ping.log" 2>&1
    [ -n "$pong_pid" ] && kill -INT "$pong_pid"; wait $pong
    ;;
tput_*)
    PT=$PERF_WS/performance_test/lib/performance_test/perf_test
    common="-c ROS2 -t Array4k --dds_domain_id 83 --keep_last --history_depth 1"
    # Two CPUs each: the perf_test subscriber pinned to one CPU spun its main thread for minutes and never printed a
    # row (PC, 2026-10-21, arm A), so the tput processes get a pair.
    ps_ sub "$CPU_S,$((CPU_S + 2))" timeout -s INT -k 5 $((DUR + 30)) "$PT" $common -p 0 -s 1 --expected_num_pubs 1 --max_runtime $((DUR + 6)) --ros-args --param start_type_description_service:=false >"$RUN/sub.log" 2>&1 &
    sub=$!
    sleep 2
    sub_pid=$(node_of $sub perf_test)
    grep -o "[^ ]*librmw_tickle.so" /proc/$sub_pid/maps | sort -u >"$RUN/sub.maps"
    ps_ pub "$CPU_C,$((CPU_C + 2))" timeout -s INT -k 5 $((DUR + 30)) "$PT" $common -r 0 -p 1 -s 0 --expected_num_subs 1 --max_runtime "$DUR" --ros-args --param start_type_description_service:=false >"$RUN/pub.log" 2>&1
    wait $sub
    ;;
esac
chown -R "$OWNER" "$RUN"'

run_one() { # label install cell rep
    local l=$1 inst=$2 cell=$3 rep=$4 run
    run="$OUT.d/${cell}_${l}_r${rep}"
    rm -rf "$run"
    mkdir -p "$run"
    echo "$inst" >"$run/install.txt"
    NS=slotcost_$$
    sudo -n ip netns add "$NS" || { say "FATAL: netns"; exit 1; }
    sudo -n ip -n "$NS" link set lo up multicast on
    sudo -n ip -n "$NS" route add default dev lo
    sudo -n ip netns exec "$NS" env -i PATH="$PATH" HOME="$HOME" INSTALL="$inst" RUN="$run" CELL="$cell" DUR="$DUR" \
        RTT_I="$RTT_I" CPU_C="$CPU_C" CPU_S="$CPU_S" EVENTS="$EVENTS" PERF_WS="$PERF_WS" IFACES="$IFACES" OWNER="$(id -u):$(id -g)" \
        bash -c "$INNER"
    local rc=$?
    sudo -n ip netns del "$NS"
    NS=""
    say "  $(date +%T) $cell $l r$rep exit $rc load $(cut -d' ' -f1 /proc/loadavg)"
}

for rep in $(seq 1 "$REPS"); do
    order=("${arm_list[@]}")
    for ((i = ${#arm_list[@]} - 1; i >= 0; i--)); do order+=("${arm_list[$i]}"); done
    k=0
    for a in "${order[@]}"; do
        k=$((k + 1))
        for cell in $CELLS; do
            run_one "${a%%=*}" "${a#*=}" "$cell" "${rep}_$k"
        done
    done
done

python3 "$HERE/slotloans_cost_pc.py" "$OUT.d" "${labels[@]}" | tee -a "$SUM"
exit "${PIPESTATUS[0]}"
