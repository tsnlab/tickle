#!/usr/bin/env bash
# holdover_tput_pc.sh - one same-host rmw_tickle tput cell (apex perf_test, -r 0) on the PC, in a private netns with a
# private /dev/shm, for the executor hold-over's A/B (ROADMAP "rmw kernel wake / hand-off").
#
# Usage: holdover_tput_pc.sh OUT_DIR INSTALL PERF_INSTALL MSG QOS DUR [ENV=VAL ...]
#   INSTALL       a pc_preflight_build.sh install (rmw_tickle + typesupports)
#   PERF_INSTALL  the matching perf/install
#   ENV=VAL       extra environment for BOTH processes (e.g. RMW_TICKLE_EXECUTOR_HOLDOVER=0)
#   PERF_REC=stat   perf stat of the subscriber's getsockopt() and timerfd_settime() over 3 s (perf_stat.log);
#   PERF_REC=1      perf record -g of the subscriber over 3 s (perf.data)
# Leaves sub.log, pub.log, maps.txt (the librmw_tickle.so each process mapped) in OUT_DIR. Prints one line, always:
# "CELL rc=<n> out=<dir>". holdover_tput_ab.sh drives it and reads the runs; this script decides nothing.
set -uo pipefail
OUT=$1 INSTALL=$2 PERF=$3 MSG=$4 QOS=$5 DUR=$6
shift 6
EXTRA=("$@")
rc=1
ns=hotput_$$
trap 'sudo -n ip netns del "$ns" 2>/dev/null; echo "CELL rc=$rc out=$OUT"' EXIT
rm -rf "$OUT"
mkdir -p "$OUT" || exit
sudo -n ip netns add "$ns" || { echo "cannot create netns"; exit; }
sudo -n ip -n "$ns" link set lo up multicast on
sudo -n ip -n "$ns" route add default dev lo
# Root inside the namespace (sudo permits only ip): a private /dev/shm, then both processes as that root, niced.
sudo -n ip netns exec "$ns" env -i PATH="$PATH" HOME="$HOME" OUT="$OUT" INSTALL="$INSTALL" PERF="$PERF" MSG="$MSG" \
    QOS="$QOS" DUR="$DUR" PERF_REC="${PERF_REC:-}" OWNER="$(id -u):$(id -g)" "${EXTRA[@]}" bash -c '
mount -t tmpfs -o size=512m tmpfs /dev/shm || exit 3
set +u
source /opt/ros/lyrical/setup.bash
source "$INSTALL/local_setup.bash"
source "$PERF/local_setup.bash"
export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=127.255.255.255
PT=$PERF/performance_test/lib/performance_test/perf_test
common=(-c ROS2 -t "$MSG" --dds_domain_id 79)
if [ "$QOS" = reliable ]; then common+=(--reliable --history_depth 1000); else common+=(--keep_last --history_depth 1); fi
rosargs=(--ros-args --param start_type_description_service:=false)
cd "$OUT"
nice -n 5 "$PT" "${common[@]}" -p 0 -s 1 --expected_num_pubs 1 --max_runtime $((DUR + 6)) "${rosargs[@]}" >sub.log 2>&1 &
s=$!
sleep 2
nice -n 5 "$PT" "${common[@]}" -r 0 -p 1 -s 0 --expected_num_subs 1 --max_runtime "$DUR" "${rosargs[@]}" >pub.log 2>&1 &
p=$!
sleep 1
grep -o "/[^ ]*librmw_tickle.so" /proc/$s/maps | sort -u | sed "s/^/sub /" >maps.txt
grep -o "/[^ ]*librmw_tickle.so" /proc/$p/maps | sort -u | sed "s/^/pub /" >>maps.txt
if [ "${PERF_REC:-}" = stat ]; then perf stat -e syscalls:sys_enter_getsockopt,syscalls:sys_enter_timerfd_settime -p $s -- sleep 3 >perf_stat.log 2>&1;
elif [ -n "${PERF_REC:-}" ]; then perf record -e cpu-clock -F 4999 -g -o perf.data -p $s -- sleep 3 >perf_rec.log 2>&1; fi
wait $p; wait $s
chown -R "$OWNER" "$OUT"
' && rc=0
