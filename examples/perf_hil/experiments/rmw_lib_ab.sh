#!/usr/bin/env bash
# rmw_lib_ab.sh - two builds of librmw_tickle.so against each other on the veth ping-pong, interleaved, without a
# rebuild between runs: each run copies its arm's library over this checkout's installed one, and checks the md5 it
# then loads (RMW_PERF_PLAN.md 9.3, 2026-09-27). Per run: the pong starts, 4 s idle, then the ping (BEST_EFFORT
# bench, --wait block) at a 5 ms gap for 5 s and at the scored 100 ms gap for 10 s. Reported per run: the RTT median
# from --stamps, the pong's whole-run CPU (its threads' schedstat, read by its own root shell just before it is
# stopped - its PID is the one $! gives there, not a search) and its park_wakes= from the shutdown line.
#
# Usage: rmw_lib_ab.sh <lib A> <lib B> <reps>     Results: $OUT (default /tmp/rmw_lib_ab.txt), "DONE" at the end.
# PC-only (veth between two private netns), so no rig lock; run it detached, it takes ~1 min a rep.
set -u
LIB_A=${1:?usage: rmw_lib_ab.sh <lib A> <lib B> <reps>}
LIB_B=${2:?}
REPS=${3:?}
OUT=${OUT:-/tmp/rmw_lib_ab.txt}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="$REPO/install/rmw_perf_pingpong/lib/rmw_perf_pingpong"
LIB="$REPO/install/rmw_tickle/lib/librmw_tickle.so"
KEEP="$(mktemp /tmp/rmw_lib_ab_keep.XXXXXX)"
cp "$LIB" "$KEEP"
NS1=la-ns1
NS2=la-ns2
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    cp "$KEEP" "$LIB" && rm -f "$KEEP" # the checkout's own build goes back
    return 0
}
trap cleanup EXIT
trap 'exit 130' INT TERM
setup_ns() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add la1 type veth peer name la2 || exit 1
    sudo -n ip link set la1 netns "$NS1"
    sudo -n ip link set la2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev la1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev la2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set la1 up
    sudo -n ip -n "$NS2" link set la2 up
}

run_one() { # <arm A|B> <lib> <gap> <duration>
    local arm=$1 lib=$2 gap=$3 dur=$4
    cp "$lib" "$LIB" || exit 1
    local loaded
    loaded=$(md5sum <"$LIB" | cut -c1-8)
    [ "$loaded" = "$(md5sum <"$lib" | cut -c1-8)" ] || { echo "arm $arm: the copy did not take" >&2; exit 1; }
    setup_ns
    local env=". /opt/ros/lyrical/setup.bash; . $REPO/install/setup.bash; export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_HOME=/tmp/la_roshome"
    local started
    started=$(date +%s)
    rm -f /tmp/la_ping_done
    # shellcheck disable=SC2024 # the logs are this shell's
    sudo -n ip netns exec "$NS2" timeout $((dur + 20)) bash -c "$env; $BIN/pong_node -m bench & p=\$!; until [ -e /tmp/la_ping_done ]; do sleep 0.05; done; cat /proc/\$p/task/*/schedstat > /tmp/la_pong_sched.txt; kill -INT \$p; wait \$p" >/tmp/la_pong.log 2>&1 &
    local pong=$!
    sleep 4
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "$env; timeout -s INT $((dur + 5)) $BIN/ping_node -d $dur -i $gap --wait block -m bench --stamps /tmp/la_stamps.txt" >/tmp/la_ping.log 2>&1
    touch /tmp/la_ping_done
    wait "$pong"
    local rtt=stale cpu=na
    [ "$(stat -c %Y /tmp/la_stamps.txt 2>/dev/null || echo 0)" -ge "$started" ] &&
        rtt=$(awk '!/^#/ && NF == 3 { print ($3 - $2) / 1000 }' /tmp/la_stamps.txt | sort -n |
            awk '{ a[NR] = $1 } END { if (NR) printf "%.1f", a[int((NR + 1) / 2)]; else print "na" }')
    [ "$(stat -c %Y /tmp/la_pong_sched.txt 2>/dev/null || echo 0)" -ge "$started" ] &&
        cpu=$(awk '{ t += $1 } END { printf "%.2f", t / 1e6 }' /tmp/la_pong_sched.txt)
    echo "arm=$arm lib=$loaded gap=$gap rtt_median_us=$rtt pong_cpu_ms=$cpu pong_$(grep -o 'park_wakes=[0-9]*' /tmp/la_pong.log | tail -1)" >>"$OUT"
}

: >"$OUT"
echo "rmw_lib_ab $(date -Is) A=$LIB_A B=$LIB_B reps=$REPS" >>"$OUT"
for _ in $(seq 1 "$REPS"); do
    for gap in 0.005 0.1; do
        dur=5
        [ "$gap" = 0.1 ] && dur=10
        run_one A "$LIB_A" "$gap" "$dur"
        run_one B "$LIB_B" "$gap" "$dur"
    done
done
echo "DONE" >>"$OUT"
