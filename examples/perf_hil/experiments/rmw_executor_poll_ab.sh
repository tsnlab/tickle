#!/usr/bin/env bash
# Executor-driven receive (RMW_TICKLE_EXECUTOR_POLL=1, 2026-09-26, rmw_tickle/RMW_PERF_PLAN.md): does taking
# the poll thread -> executor wake off the pong's and ping's receive path shorten the rmw round trip, at no
# CPU cost? The same build, the switch set per arm on both sides.
#
# ARMS: off / on, BEST_EFFORT bench, the rmw ping-pong on a veth pair between two private netns, interleaved,
# REPS reps. Each run: the pong starts, 4 s idle, then the ping:
#   gap5    -i 0.005 -d 5 (~1000 round trips), --wait block
#   gap100  -i 0.1 -d 10 (100 round trips, the scored rig shape: rmw_crosshost_rtt.sh, COMPARISON rows 58-59)
#   control -i 0.005 -d 5, --wait poll
#
# HOW TO READ IT, written before running (pre-registered with Plan; v3 - the poll thread parks on a lease
# timer armed at release, v1 was killed on CPU):
#   WIN (gap5 and gap100): RTT median (from --stamps) falls by >= 5 us beyond 2 SE across reps. Predicted
#     ~25 us at gap5, as v1 measured; at gap100 the ping hands the role back and forth every round trip
#     (its sleep is outside rmw_wait and longer than the 10 ms lease), which costs the ping's claim, not
#     the pong's path, so still a win, somewhat smaller.
#   CPU: the pong's schedstat CPU over the whole run (4 s idle + the pings; its process's threads summed,
#     read when the ping ends) must not rise beyond 2 SE - at gap100 that is COMPARISON row 58's metric.
#     The pong's spin() never leaves rmw_wait but for callbacks, so it pays two timerfd_settime() a message
#     and no thread switch; idle, the executor serves core's timers instead of the poll thread.
#     The ping's CPU (user+sys, /usr/bin/time) is reported too; at gap100 it may rise by the handover.
#   CONTROL (poll mode): no RTT change - rmw_wait(0) never takes the role.
#   KILL: a win under 5 us, or a pong CPU rise, and it is not merged.
#   IDENTITY: each process prints "rmw_tickle: executor_poll=<0|1> executor_poll_waits=N" at shutdown - the
#     line only this build has; the on arm must show waits > 0 in block mode. The pong's PID for schedstat
#     is found by its executable's path, not by a name pattern.
#
# SEEN (x86 veth, 2026-09-26, 5 reps, 39570d7a + the change; mean of the per-run values, sd):
#   v1 (poll thread re-armed by a futex signal on every release, 1 ms lease): RTT -26 / -30 us (bench /
#     array1k, -8..-12 SE), but CPU per round trip +49 us (+10 SE) - killed. The signal and two poll-thread
#     wakes per message on the pong, and a handover per round trip on the ping.
#   v3 (timerfd lease armed on release, no thread woken, 10 ms):
#     gap5     RTT median 100.1 (2.8) -> 65.6 (4.0) us, -34.5 (-16 SE); pong CPU 65.3 -> 53.0 ms (-19%, -11 SE)
#     gap100   RTT median 216.3 (8.6) -> 169.4 (27.9) us, -47 (-3.6 SE; 4 of 5 on-runs 149-166, one 218 in a
#              rep where off was also its slowest); pong CPU 28.7 -> 27.4 ms (-1.9 SE); ping CPU +10 ms per
#              run (+4 SE) - its handover each round trip, as predicted, and not a scored metric
#     control  RTT 178.7 -> 183.5 us (+1.0 SE, no change); pong CPU 64.2 -> 55.1 ms (its spin() polls anyway)
#   Pre-registration met: the win is far past 5 us at both gaps, the pong's CPU falls. The rig A/B (Plan) is
#   the merge gate.
#
# Usage: rmw_executor_poll_ab.sh <reps>     Results: $OUT (default /tmp/rmw_executor_poll_ab.txt)
set -u
REPS=${1:?usage: rmw_executor_poll_ab.sh <reps>}
OUT=${OUT:-/tmp/rmw_executor_poll_ab.txt}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="$REPO/install/rmw_perf_pingpong/lib/rmw_perf_pingpong"
NS1=rep-ns1
NS2=rep-ns2
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

setup_ns() {
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add rep1 type veth peer name rep2 || exit 1
    sudo -n ip link set rep1 netns "$NS1"
    sudo -n ip link set rep2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev rep1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev rep2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set rep1 up
    sudo -n ip -n "$NS2" link set rep2 up
}

median_rtt_us() { # <stamps file> - rows "seq begin_ns end_ns"
    awk '!/^#/ && NF == 3 { print ($3 - $2) / 1000 }' "$1" | sort -n | awk '{ a[NR] = $1 } END { if (NR) printf "%.1f", a[int((NR + 1) / 2)]; else print "na" }'
}

run_one() { # <on 0|1> <arm gap5|gap100|control>
    local on=$1 arm=$2 mode=block gap=0.005 dur=5
    [ "$arm" = gap100 ] && { gap=0.1; dur=10; }
    [ "$arm" = control ] && mode=poll
    setup_ns
    local env=". /opt/ros/lyrical/setup.bash; . $REPO/install/setup.bash; export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_HOME=/tmp/rep_roshome RMW_TICKLE_EXECUTOR_POLL=$on"
    rm -f /tmp/rep_ping_done
    local started
    started=$(date +%s)
    # shellcheck disable=SC2024 # the logs are this shell's
    # The pong runs as root (ip netns exec) and this user cannot read root's /proc, so its own root shell
    # reads the pong's schedstat - all threads - when the ping is done (/tmp/rep_ping_done), then stops it.
    # Its PID is the one $! gives that shell, not a search.
    sudo -n ip netns exec "$NS2" timeout $((dur + 15)) bash -c "$env; $BIN/pong_node -m bench & p=\$!; until [ -e /tmp/rep_ping_done ]; do sleep 0.05; done; cat /proc/\$p/task/*/schedstat > /tmp/rep_pong_sched.txt; kill -INT \$p; wait \$p" >/tmp/rep_pong.log 2>&1 &
    local pong=$!
    sleep 4 # 4 s idle
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "$env; exec /usr/bin/time -f 'cpu_s=%U+%S' -o /tmp/rep_ping_time.txt timeout -s INT $((dur + 5)) $BIN/ping_node -d $dur -i $gap --wait $mode -m bench --stamps /tmp/rep_stamps.txt" >/tmp/rep_ping.log 2>&1
    touch /tmp/rep_ping_done
    wait "$pong"
    local pong_cpu=na
    # Only this run's files: the root-owned ones cannot be removed beforehand, so check they are fresh.
    [ "$(stat -c %Y /tmp/rep_pong_sched.txt 2>/dev/null || echo 0)" -ge "$started" ] &&
        pong_cpu=$(awk '{ t += $1 } END { printf "%.2f", t / 1e6 }' /tmp/rep_pong_sched.txt)
    local ping_cpu
    ping_cpu=$(grep -o 'cpu_s=[0-9.+]*' /tmp/rep_ping_time.txt | cut -d= -f2 | bc 2>/dev/null)
    echo "on=$on arm=$arm $(grep -h '^RESULT' /tmp/rep_ping.log | tr ' ' '\n' | grep -E '^(sent|recv|rtt_min_ms|rtt_avg_ms)=' | paste -sd' ') rtt_median_us=$( [ "$(stat -c %Y /tmp/rep_stamps.txt 2>/dev/null || echo 0)" -ge "$started" ] && median_rtt_us /tmp/rep_stamps.txt || echo stale) pong_cpu_ms=$pong_cpu ping_cpu_s=${ping_cpu:-na} ping_id=\"$(grep -h 'executor_poll=' /tmp/rep_ping.log | tail -1 | sed 's/rmw_tickle: //')\" pong_id=\"$(grep -h 'executor_poll=' /tmp/rep_pong.log | tail -1 | sed 's/rmw_tickle: //')\"" >>"$OUT"
}

: >"$OUT"
echo "build: $(git -C "$REPO" rev-parse --short HEAD) + working tree" >>"$OUT"
for _ in $(seq 1 "$REPS"); do
    for arm in gap5 gap100 control; do
        for on in 0 1; do
            run_one "$on" "$arm"
        done
    done
done
echo "DONE" >>"$OUT"
