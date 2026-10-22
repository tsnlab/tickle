#!/usr/bin/env bash
# run.sh - bundle10_rss scenarios: which of a context's tables become resident, per build, scenario and role.
#
# Two private netns joined by a veth pair (10.77.0.1 and .2, broadcast 10.77.0.255): two processes on different
# addresses talk over UDP, as two hosts do. Nothing runs in the PC's default netns. Every process runs as this user
# (setpriv after `ip netns exec`), niced, with ulimit -v 2 GiB and -f 200 MiB.
#
# Scenarios (name:topology:roleA:roleB:msg:rate_hz:loss%). roleA runs in ns A, roleB in ns B ("net") or also in ns A
# ("shm": the same address, so the same-host segment carries the data). B starts first and runs DUR+3 s; A starts
# 1 s later and runs DUR s. QoS RELIABLE KEEP_LAST 10 everywhere (each build's default for the core, set for rclcpp).
#   idle          one context alone, nothing created on it
#   pubsub_net    small messages over UDP          pubsub_shm  the same on one host (segment)
#   reliable_loss small messages, netem loss 1% on both veth ends
#   frag_mid      32 KiB messages: DATA_FRAG reassembly (rmw only: the core default has a 1472-byte sample)
#   large_1m      1 MiB messages: the large-sample path (rmw only)
#   frag_loss, large_loss  the same two with netem loss 1% (a lost fragment is what would send a sample to a slot)
#   services      a client calling a server at 100 Hz
#
# Outputs per run OUT/<build>/<scenario>_r<rep>_<A|B>.{log,rt}; the .rt holds the RT lines rss_tables.c prints. Read
# them with read.py, whose rules were written before the first run.
#
# Usage: OUT=<dir> [BUILDS="core rmw"] [SCENARIOS=...] [REPS=2] [DUR=10] [B=/home/semih/tmp_build/b10rss] run.sh
set -uo pipefail
B=${B:-/home/semih/tmp_build/b10rss}
WS=${WS:-$B/ws}
OUT=${OUT:?results directory}
BUILDS=${BUILDS:-"core rmw"}
REPS=${REPS:-2}
DUR=${DUR:-10}
SCENARIOS=${SCENARIOS:-"idle:net:idle:-:small:1:0 pubsub_net:net:pub:sub:small:1000:0 pubsub_shm:shm:pub:sub:small:1000:0 reliable_loss:net:pub:sub:small:1000:1 frag_mid:net:pub:sub:mid:100:0 large_1m:net:pub:sub:large:10:0 frag_loss:net:pub:sub:mid:100:1 large_loss:net:pub:sub:large:10:1 services:net:client:server:small:100:0"}
mkdir -p "$OUT" || exit 1
NA=b10a_$$
NB=b10b_$$
MY_UID=$(id -u)
MY_GID=$(id -g)

cleanup() {
    sudo -n ip netns del "$NA" 2>/dev/null
    sudo -n ip netns del "$NB" 2>/dev/null
    echo "BUNDLE10_RSS END $(date -Is)"
}
trap cleanup EXIT

sudo -n ip netns add "$NA" || { echo "cannot create netns"; exit 1; }
sudo -n ip netns add "$NB" || exit 1
sudo -n ip -n "$NA" link add va type veth peer name vb netns "$NB" || exit 1
sudo -n ip -n "$NA" addr add 10.77.0.1/24 broadcast 10.77.0.255 dev va
sudo -n ip -n "$NB" addr add 10.77.0.2/24 broadcast 10.77.0.255 dev vb
for ns in "$NA" "$NB"; do
    sudo -n ip -n "$ns" link set lo up
done
sudo -n ip -n "$NA" link set va up
sudo -n ip -n "$NB" link set vb up
sudo -n ip -n "$NA" route add default dev va
sudo -n ip -n "$NB" route add default dev vb

set_loss() {
    for pair in "$NA:va" "$NB:vb"; do
        # tc through `ip netns exec`: sudoers here permits ip alone, and a bare `sudo -n tc` fails.
        sudo -n ip netns exec "${pair%%:*}" tc qdisc del dev "${pair#*:}" root 2>/dev/null
        if [ "$1" != 0 ]; then
            sudo -n ip netns exec "${pair%%:*}" tc qdisc add dev "${pair#*:}" root netem loss "$1%" || return 1
            sudo -n ip netns exec "${pair%%:*}" tc qdisc show dev "${pair#*:}" | grep -q "loss $1%" || return 1
        fi
    done
}

# run_in NS LOG cmd... - as this user, in NS, limited; background, PID in $!.
run_in() {
    local ns=$1 log=$2
    shift 2
    # shellcheck disable=SC2024 # the log is written by this user on purpose, not by root
    sudo -n ip netns exec "$ns" setpriv --reuid="$MY_UID" --regid="$MY_GID" --clear-groups \
        bash -c 'ulimit -v 2097152; ulimit -f 204800; exec nice -n 10 "$@"' _ "$@" >"$log" 2>&1 </dev/null &
}

# role_cmd BUILD ROLE MSG RATE DUR ID RT_FILE - the command line of one role.
role_cmd() {
    local build=$1 role=$2 msg=$3 rate=$4 dur=$5 id=$6 rt=$7
    case "$build" in
    core) echo "$B/core_host $B/layout_core.txt $role $id 10.77.0.255 $rate $dur" ;;
    rmw)
        # shellcheck disable=SC2016 # expanded by the role's own shell
        printf '%s' "env -i PATH=/usr/bin:/bin HOME=$HOME bash -c 'source /opt/ros/lyrical/setup.bash && source $WS/install/setup.bash && exec env RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=10.77.0.255 TICKLE_NODE_ID=$id ROS_DOMAIN_ID=77 RSS_TABLES_LAYOUT=$B/layout_rmw.txt RSS_TABLES_OUT=$rt LD_PRELOAD=$B/rss_tables_preload.so $WS/install/rss_tables_rmw/lib/rss_tables_rmw/rss_node $role $msg reliable $rate $dur'"
        ;;
    esac
}

for rep in $(seq 1 "$REPS"); do
    for build in $BUILDS; do
        mkdir -p "$OUT/$build"
        for sc in $SCENARIOS; do
            IFS=: read -r name topo ra rb msg rate loss <<<"$sc"
            if [ "$build" = core ] && [ "$msg" != small ]; then
                continue
            fi
            base=$OUT/$build/${name}_r$rep
            set_loss "$loss" || { echo "netem failed"; exit 1; }
            echo "RUN $(date +%T) build=$build scenario=$name rep=$rep loss=$loss"
            pid_b=""
            if [ "$rb" != - ]; then
                nsb=$NB
                [ "$topo" = shm ] && nsb=$NA
                eval "run_in $nsb $base""_B.log $(role_cmd "$build" "$rb" "$msg" "$rate" $((DUR + 3)) 12 "${base}_B.rt")"
                pid_b=$!
                sleep 1
            fi
            eval "run_in $NA $base""_A.log $(role_cmd "$build" "$ra" "$msg" "$rate" "$DUR" 11 "${base}_A.rt")"
            pid_a=$!
            wait "$pid_a"
            echo "  A exit $?"
            if [ -n "$pid_b" ]; then
                wait "$pid_b"
                echo "  B exit $?"
            fi
            # The treatment as applied: netem's own drop counters on both ends (read.py voids a loss run with none).
            for pair in "$NA:va" "$NB:vb"; do
                sudo -n ip netns exec "${pair%%:*}" tc -s qdisc show dev "${pair#*:}"
            done >"${base}_netem.txt" 2>&1
            # The core host prints its RT lines on stdout; the rmw preload writes its own file.
            for s in A B; do
                if [ "$build" = core ] && [ -f "${base}_$s.log" ]; then
                    grep -E '^(RT|RT_PROC|CORE_HOST) ' "${base}_$s.log" >"${base}_$s.rt"
                fi
            done
        done
    done
done
set_loss 0
