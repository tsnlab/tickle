#!/usr/bin/env bash
# stepb_ctl_pc.sh - the step B rig control cell on the PC: perf_test Array16k, same host (two processes in ONE private
# network namespace), BEST_EFFORT KEEP_LAST 5, 30 Hz, for each arm in ARMS, interleaved and rotated per rep. Written
# 2026-10-15 to find why the rig's control (largemsgB_compare.txt: subscriber CPU/MB +5% on step B, 5 of 5 reps) moved.
#
# Each perf_test runs under `perf stat` (task-clock, instructions:u/k, cycles:u/k) and ulimit -v 2 GiB; the arm's
# librmw_tickle.so is read back from the subscriber's /proc/PID/maps (a run whose maps do not show the arm's own library
# is VOID and says so). Nothing here touches the rig, its lock or the default namespace; it holds no PC-preflight lock
# (its /tmp files are its own: $OUT).
#
#   ARMS="pre=/path/install head=/path/install ..."  each install is sourced over BASE_INSTALL (as tickle@pre is on the
#                                                     rig); BASE_INSTALL and PERF_WS give perf_test and its typesupports
#   REPS(8) DUR(30) RATE(30) TOPIC(Array16k) QOS("--keep_last --history_depth 5") OUT(dir, required)
#   SUB_PREFIX: words put before the subscriber's perf_test inside perf stat, @STEM@ standing for the run's stem
#               (e.g. "strace -f -c -o @STEM@.strace")
#   PERF_RECORD=1: the subscriber under `perf record $PERF_RECORD_ARGS` (default -g) instead of perf stat
#
# Output: $OUT/<arm>_r<rep>_{sub,pub}.{log,perf,maps}, the subscriber's per-thread CPU (.threads) and memory (.smaps,
# .status: the last snapshot); $OUT/summary.txt - one RUN line per run, then stepb_ctl_read.py's reading (per arm
# mean, SE, n; each arm against the first, paired by rep). sub_main_ms is perf_test's main thread - its AnalyzeRunner's
# SpinLock wait, not the rmw (largemsg_control_spin.py's docstring) - and sub_other_ms every other thread of it.
# Last line always: STEPB_CTL_PC rc=<n>.
set -uo pipefail
: "${OUT:?set OUT: a results directory outside the session}"
: "${ARMS:?set ARMS: name=install ...}"
BASE_INSTALL=${BASE_INSTALL:-/home/semih/pc_preflight_builds/003f71f880e0/install}
PERF_WS=${PERF_WS:-/home/semih/pc_preflight_builds/003f71f880e0/perf/install}
REPS=${REPS:-8} DUR=${DUR:-30} RATE=${RATE:-30} TOPIC=${TOPIC:-Array16k}
QOS=${QOS:---keep_last --history_depth 5}
NS=stepbctl_$$ PEER=stepbctp_$$ VETH=sbc$$
rc=1
mkdir -p "$OUT" || exit 1
SUM=$OUT/summary.txt
cleanup() {
    for f in "$OUT"/.pid_*; do
        [ -f "$f" ] || continue
        p=$(cat "$f")
        case "$(readlink "/proc/$p/exe" 2>/dev/null)" in */perf_test | */perf) kill -INT "$p" 2>/dev/null ;; esac
        rm -f "$f"
    done
    sudo -n ip netns del "$NS" 2>/dev/null
    sudo -n ip netns del "$PEER" 2>/dev/null
    echo "STEPB_CTL_PC rc=$rc $(date -Is)" | tee -a "$SUM"
}
trap cleanup EXIT

if ! { sudo -n ip netns add "$NS" && sudo -n ip netns add "$PEER"; }; then
    echo "cannot add namespaces"
    rc=2
    exit
fi
sudo -n ip link add "${VETH}a" type veth peer name "${VETH}b" || { rc=2; exit; }
for spec in "$NS:${VETH}a:192.168.10.2" "$PEER:${VETH}b:192.168.10.3"; do
    IFS=: read -r ns dev ip <<<"$spec"
    if ! { sudo -n ip link set "$dev" netns "$ns" && sudo -n ip -n "$ns" link set "$dev" name eth0 &&
        sudo -n ip -n "$ns" addr add "$ip/24" brd 192.168.10.255 dev eth0 && sudo -n ip -n "$ns" link set lo up &&
        sudo -n ip -n "$ns" link set eth0 up && sudo -n ip -n "$ns" route add default dev eth0; }; then
        rc=2
        exit
    fi
done

PT=$PERF_WS/performance_test/lib/performance_test/perf_test
EVENTS=task-clock,instructions:u,instructions:k,cycles:u,cycles:k
in_ns() { sudo -n ip netns exec "$NS" sudo -n -u "$USER" bash -c "$1"; }

# $1 arm $2 install $3 role $4 perf_test args $5 stem; runs detached in the namespace, writes perf_test's PID
launch() {
    local env="set +u; source /opt/ros/lyrical/setup.bash; source $BASE_INSTALL/local_setup.bash
source $PERF_WS/local_setup.bash; source $2/local_setup.bash; set -u
export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_DOMAIN_ID=77 TMPDIR=/home/semih/tmp_build
ulimit -v 2097152"
    local pre="perf stat -x, -o $5.perf -e $EVENTS --"
    if [ "$3" = sub ] && [ "${PERF_RECORD:-0}" = 1 ]; then pre="perf record ${PERF_RECORD_ARGS:--g} -o $5.data --"; fi
    if [ "$3" = sub ] && [ -n "${SUB_PREFIX:-}" ]; then pre="$pre ${SUB_PREFIX//@STEM@/$5}"; fi
    in_ns "$env
cd $OUT; setsid $pre $PT $4 > $5.log 2>&1 < /dev/null & echo \$! > $OUT/.launcher_$3" </dev/null
    local l p="" kids d c
    l=$(cat "$OUT/.launcher_$3")
    for _ in $(seq 1 50); do # perf_test is perf's (or strace's) descendant: found by /proc exe, never by pattern
        kids=$(cat /proc/"$l"/task/*/children 2>/dev/null)
        for d in $kids; do kids="$kids $(cat /proc/"$d"/task/*/children 2>/dev/null)"; done
        for c in $kids; do
            case "$(readlink "/proc/$c/exe" 2>/dev/null)" in */perf_test) p=$c ;; esac
        done
        [ -n "$p" ] && break
        sleep 0.1
    done
    echo "${p:-$l}" > "$OUT/.pid_$3"
    echo "$l" > "$OUT/.lpid_$3"
}
wait_gone() { # $1 role $2 seconds: the launcher (perf) gone
    local l t=0
    l=$(cat "$OUT/.lpid_$1")
    while [ -d "/proc/$l" ] && [ "$t" -lt "$(($2 * 10))" ]; do sleep 0.1; t=$((t + 1)); done
    [ -d "/proc/$l" ] && { kill -INT "$(cat "$OUT/.pid_$1")" 2>/dev/null; sleep 3; }
    rm -f "$OUT/.pid_$1"
}

read -r -a arm_list <<<"$ARMS"
echo "=== stepb_ctl_pc $(date -Is): arms [$ARMS] reps $REPS dur $DUR rate $RATE topic $TOPIC qos [$QOS] base $BASE_INSTALL" \
    "load [$(cut -d' ' -f1-3 /proc/loadavg)] ===" | tee "$SUM"
n=${#arm_list[@]}
for rep in $(seq 1 "$REPS"); do
    for k in $(seq 0 $((n - 1))); do
        spec=${arm_list[$(((k + rep - 1) % n))]}
        arm=${spec%%=*} inst=${spec#*=}
        stem=$OUT/${arm}_r$rep
        common="-c ROS2 -t $TOPIC $QOS --dds_domain_id 77"
        ros="--ros-args --param start_type_description_service:=false"
        load0=$(cut -d' ' -f1 /proc/loadavg)
        launch "$arm" "$inst" sub "$common -p 0 -s 1 --expected_num_pubs 1 --max_runtime $((DUR + 8)) $ros" "${stem}_sub"
        sleep 2
        spid=$(cat "$OUT/.pid_sub")
        grep -o '[^ ]*librmw_tickle.so' "/proc/$spid/maps" 2>/dev/null | sort -u > "${stem}_sub.maps"
        # Per-thread CPU (on-CPU ns, the first field of each /proc/PID/task/TID/schedstat), the last snapshot before
        # perf_test exits:
        # perf_test's main thread runs AnalyzeRunner, whose sync_reset() busy-waits on the SpinLock the subscriber's
        # DataRunner thread holds across spin_once() - so the main thread's CPU is perf_test's wait, not the rmw's.
        (while [ -d "/proc/$spid" ]; do
            for t in /proc/"$spid"/task/*; do
                tid=${t##*/}
                read -r ns _ < "$t/schedstat" 2>/dev/null || continue
                echo "$tid $ns"
            done > "${stem}_sub.threads.tmp" && mv "${stem}_sub.threads.tmp" "${stem}_sub.threads"
            # Its memory as anonymous and shared (a step B reader's mapped region chunks are RssShmem)
            grep -E '^(Rss|Pss|Pss_Anon|Pss_Shmem|Rss_?Anon|Rss_?Shmem|Anonymous|Shared_Dirty|Private_Dirty):' \
                "/proc/$spid/smaps_rollup" > "${stem}_sub.smaps.tmp" 2>/dev/null &&
                mv "${stem}_sub.smaps.tmp" "${stem}_sub.smaps"
            grep -E '^Vm(HWM|RSS):|^Rss(Anon|File|Shmem):' "/proc/$spid/status" > "${stem}_sub.status.tmp" 2>/dev/null &&
                mv "${stem}_sub.status.tmp" "${stem}_sub.status"
            sleep 0.5
        done) &
        launch "$arm" "$inst" pub "$common -r $RATE -p 1 -s 0 --expected_num_subs 1 --max_runtime $DUR $ros" "${stem}_pub"
        wait_gone pub $((DUR + 30))
        wait_gone sub 25
        lib=$(head -1 "${stem}_sub.maps")
        ok=VALID
        case "$lib" in "$inst"/*) ;; *) ok="VOID(maps:${lib:-none})" ;; esac
        get() { awk -F, -v e="$2" '$3 == e || $3 ~ "^"e"$" {print $1}' "$1" | head -1; }
        line="RUN arm=$arm rep=$rep $ok load=$load0"
        for e in task-clock instructions:u instructions:k cycles:u cycles:k; do
            line="$line sub_$e=$(get "${stem}_sub.perf" "$e") pub_$e=$(get "${stem}_pub.perf" "$e")"
        done
        recv=$(grep -oE ' rx_shm=[0-9]+' "${stem}_sub.log" | tail -1 | tr -d ' ')
        thr=$(awk -v p="$spid" '$1 == p {m += $2; next} {o += $2} END {printf "sub_main_ms=%.2f sub_other_ms=%.2f", m / 1e6, o / 1e6}' \
            "${stem}_sub.threads" 2>/dev/null)
        mem=$(awk '{printf "sub_%s=%s ", $1, $2}' "${stem}_sub.status" 2>/dev/null | tr -d ':')
        echo "$line $recv $thr $mem" | tee -a "$SUM"
    done
done
python3 "$(dirname "$0")/stepb_ctl_read.py" "$SUM" | tee -a "$SUM"
rc=0
