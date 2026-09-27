#!/usr/bin/env bash
# rmw_tickle's gap acceptance tests (2026-09-27, Plan; COMPARISON.md 2.7a, CONTEXT_NODE_PLAN roadmap g1-g6 and stage 3).
# Each test is one ordinary ROS 2 scenario, run first on rmw_cyclonedds_cpp - the CONTROL, which must pass, or the test
# is VOID - and then on rmw_tickle. A gap is closed when rmw_tickle passes its test; until then it is expected to fail,
# and a pass there before the fix is a finding in itself (the test cannot fail, or the gap was not what we thought).
#
# Two processes in two private network namespaces joined by a veth pair (10.77.0.1 / 10.77.0.2), so "remote" really is
# another host as far as every rmw can tell, and nothing leaks onto this PC's LANs (a TickLE node in the default netns
# reaches the rig - see the project notes).
#
#   graph    ros2 node list / node info / param get against a node in the other namespace       (stage 3)
#   bag      ros2 bag record, then ros2 bag play into a listener in the other namespace            (g1, serialized messages)
#   events   a listener on rclpy's EventsExecutor receives the other side's talker                 (g2, on-new-data callbacks)
#   matched  PUBLICATION_MATCHED / SUBSCRIPTION_MATCHED fire on both sides                          (g3)
#   itype    PUBLISHER_ / SUBSCRIPTION_INCOMPATIBLE_TYPE fire for String vs Int32 on one topic     (g3)
#   takeseq  the rmw library defines rmw_take_sequence (a symbol check, as rcl_take_sequence dispatches to it) (g5)
#   range    ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST isolates the two hosts; SUBNET does not        (g6)
#   peers    LOCALHOST plus ROS_STATIC_PEERS naming the other host connects them again            (g6)
#
# Usage: rmw_gap_acceptance.sh -w WS [TEST...]   (default: all). WS holds rmw/install (rmw_tickle) and ifaces/install
# (the standard interfaces with TickLE typesupport, build_ros2_interfaces.sh). Output: one line per test and rmw, then a
# table; exit 0 when every control passed (the rmw_tickle results are the report, not the exit status).
# The t_* tests and helpers are called indirectly, as "t_$t", and from command substitutions.
# shellcheck disable=SC2329
set -u
WS=""
while getopts "w:" o; do case "$o" in w) WS=$OPTARG ;; *) exit 2 ;; esac; done
shift $((OPTIND - 1))
[ -n "$WS" ] && [ -d "$WS/rmw/install" ] && [ -d "$WS/ifaces/install" ] || { echo "usage: $0 -w WS [TEST...]" >&2; exit 2; }
TESTS=${*:-graph bag events matched itype takeseq range peers}
HERE=$(cd "$(dirname "$0")" && pwd)
NODE="$HERE/acceptance/accept_node.py"
DISTRO=${ROS_DISTRO_DIR:-/opt/ros/lyrical}
NS1=acc-ns1
NS2=acc-ns2
OUTDIR=${OUTDIR:-$WS/acceptance_$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTDIR"
cleanup() {
    local p
    for ns in "$NS1" "$NS2"; do
        for p in $(sudo -n ip netns pids "$ns" 2>/dev/null); do sudo -n ip netns exec "$ns" kill -TERM "$p" 2>/dev/null; done
    done
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
setup_ns() {
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add accv1 type veth peer name accv2 || exit 1
    sudo -n ip link set accv1 netns "$NS1"; sudo -n ip link set accv2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 10.77.0.1/24 dev accv1; sudo -n ip -n "$NS2" addr add 10.77.0.2/24 dev accv2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set accv1 up; sudo -n ip -n "$NS2" link set accv2 up
    # multicast for CycloneDDS's SPDP over the veth
    sudo -n ip -n "$NS1" route add 224.0.0.0/4 dev accv1; sudo -n ip -n "$NS2" route add 224.0.0.0/4 dev accv2
}
# env_for RMW NSIDX EXTRA: the environment string one process runs with
env_for() {
    local rmw=$1 idx=$2 extra=${3:-} ip
    ip=10.77.0.$idx
    local e="source $DISTRO/setup.bash; export ROS_DOMAIN_ID=91 RMW_IMPLEMENTATION=$rmw ROS_LOG_DIR=$OUTDIR/roslog HOME=$OUTDIR/home"
    case "$rmw" in
        rmw_tickle) e="$e; source $WS/rmw/install/setup.bash; source $WS/ifaces/install/setup.bash; export TICKLE_BROADCAST_ADDR=10.77.0.255" ;;
        rmw_fastrtps_cpp) e="$e; export FASTDDS_BUILTIN_TRANSPORTS=UDPv4" ;; # no shared memory across the two netns
        rmw_cyclonedds_cpp) e="$e; export CYCLONEDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface address=\"$ip\"/></Interfaces></General></Domain></CycloneDDS>'" ;;
    esac
    [ -n "$extra" ] && e="$e; export $extra"
    echo "$e"
}
# in NS RMW IDX SECONDS EXTRA CMD... : run CMD in namespace NS with that environment, as this user
run_in() {
    local ns=$1 rmw=$2 idx=$3 secs=$4 extra=$5
    shift 5
    sudo -n ip netns exec "$ns" sudo -n -u "$USER" bash -c "set +u; $(env_for "$rmw" "$idx" "$extra"); set -u; timeout $((${secs%.*} + 20)) $*"
}
result() { grep -h '^RESULT:' "$1" 2>/dev/null | tail -1; }
field() { result "$1" | grep -oE "$2=[0-9]+" | cut -d= -f2; }
declare -A VERDICT
# ran LOG MIN: the helper process in LOG ended with a RESULT line and sent at least MIN - otherwise the test did not
# test anything, and says ERROR instead of FAIL.
ran() { local n; n=$(field "$1" sent); [ "${n:-0}" -ge "$2" ]; }
t_graph() {
    local rmw=$1 d=$OUTDIR/graph_$1
    mkdir -p "$d"
    run_in "$NS1" "$rmw" 1 20 "" python3 "$NODE" talker 20 > "$d/talker.log" 2>&1 &
    local tp=$!
    sleep 6
    run_in "$NS2" "$rmw" 2 10 "" ros2 node list --no-daemon > "$d/list.log" 2>&1
    run_in "$NS2" "$rmw" 2 10 "" ros2 node info /accept_talker --no-daemon > "$d/info.log" 2>&1
    run_in "$NS2" "$rmw" 2 10 "" ros2 param get /accept_talker accept_param --no-daemon > "$d/param.log" 2>&1
    wait "$tp"
    ran "$d/talker.log" 50 || { echo "ERROR(talker did not run)"; return; }
    local ok=1
    grep -qx '/accept_talker' "$d/list.log" || ok=0
    grep -q '/accept_chatter' "$d/info.log" || ok=0
    grep -q '42' "$d/param.log" || ok=0
    if [ "$ok" = 1 ]; then echo PASS; else echo "FAIL(list=$(grep -c accept_talker "$d/list.log") info=$(grep -c accept_chatter "$d/info.log") param=$(grep -c 42 "$d/param.log"))"; fi
}
t_bag() {
    local rmw=$1 d=$OUTDIR/bag_$1
    mkdir -p "$d"
    run_in "$NS1" "$rmw" 1 14 "" python3 "$NODE" talker 14 > "$d/talker.log" 2>&1 &
    local tp=$!
    sleep 3
    run_in "$NS2" "$rmw" 2 8 "" timeout -s INT 8 ros2 bag record -o "$d/bag" --topics /accept_chatter > "$d/record.log" 2>&1
    wait "$tp"
    ran "$d/talker.log" 50 || { echo "ERROR(talker did not run)"; return; }
    local recorded
    recorded=$(run_in "$NS2" "$rmw" 2 10 "" ros2 bag info "$d/bag" 2>/dev/null | grep -oE 'Messages: +[0-9]+' | grep -oE '[0-9]+' | head -1)
    run_in "$NS1" "$rmw" 1 14 "" python3 "$NODE" listener 14 > "$d/listener.log" 2>&1 &
    local lp=$!
    sleep 4
    run_in "$NS2" "$rmw" 2 10 "" ros2 bag play "$d/bag" > "$d/play.log" 2>&1
    wait "$lp"
    local got
    got=$(field "$d/listener.log" received)
    if [ "${recorded:-0}" -ge 30 ] && [ "${got:-0}" -ge 30 ]; then echo PASS; else echo "FAIL(recorded=${recorded:-0} replayed=${got:-0})"; fi
}
t_events() {
    local rmw=$1 d=$OUTDIR/events_$1
    mkdir -p "$d"
    run_in "$NS2" "$rmw" 2 12 "" python3 "$NODE" listener 12 events > "$d/listener.log" 2>&1 &
    local lp=$!
    sleep 2
    run_in "$NS1" "$rmw" 1 8 "" python3 "$NODE" talker 8 > "$d/talker.log" 2>&1
    wait "$lp"
    ran "$d/talker.log" 40 || { echo "ERROR(talker did not run)"; return; }
    local got
    got=$(field "$d/listener.log" received)
    if [ "${got:-0}" -ge 20 ]; then echo PASS; else echo "FAIL(received=${got:-0})"; fi
}
t_pair() { # role1 role2 key test
    local rmw=$1 r1=$2 r2=$3 key=$4 d=$OUTDIR/${5}_$1
    mkdir -p "$d"
    run_in "$NS1" "$rmw" 1 10 "" python3 "$NODE" "$r1" 10 > "$d/$r1.log" 2>&1 &
    local p1=$!
    run_in "$NS2" "$rmw" 2 10 "" python3 "$NODE" "$r2" 10 > "$d/$r2.log" 2>&1
    wait "$p1"
    local a b
    # rclpy raising UnsupportedEventTypeError is the gap itself - what a user's node does on this rmw - so FAIL
    if grep -q UnsupportedEventTypeError "$d/$r1.log" "$d/$r2.log"; then echo "FAIL(rclpy: UnsupportedEventTypeError)"; return; fi
    if ! result "$d/$r1.log" | grep -q . || ! result "$d/$r2.log" | grep -q .; then echo "ERROR(a node did not finish)"; return; fi
    a=$(field "$d/$r1.log" "$key"); b=$(field "$d/$r2.log" "$key")
    if [ "${a:-0}" -ge 1 ] && [ "${b:-0}" -ge 1 ]; then echo PASS; else echo "FAIL($r1=${a:-0} $r2=${b:-0})"; fi
}
t_matched() { t_pair "$1" matched_pub matched_sub matched_events matched; }
t_itype() { t_pair "$1" itype_pub itype_sub incompatible_type_events itype; }
t_takeseq() {
    local lib
    case "$1" in
        rmw_tickle) lib=$WS/rmw/install/rmw_tickle/lib/librmw_tickle.so ;;
        *) lib=$DISTRO/lib/lib$1.so ;;
    esac
    if nm -D --defined-only "$lib" 2>/dev/null | grep -qw rmw_take_sequence; then echo PASS; else echo "FAIL(rmw_take_sequence not defined in $(basename "$lib"))"; fi
}
t_range_arm() { # rmw range_value peers1 peers2 -> received count
    local rmw=$1 range=$2 p1=$3 p2=$4 d=$OUTDIR/range_${1}_$2${3:+_peers}
    mkdir -p "$d"
    local e1="ROS_AUTOMATIC_DISCOVERY_RANGE=$range" e2="ROS_AUTOMATIC_DISCOVERY_RANGE=$range"
    [ -n "$p1" ] && e1="$e1 ROS_STATIC_PEERS=$p1"
    [ -n "$p2" ] && e2="$e2 ROS_STATIC_PEERS=$p2"
    run_in "$NS2" "$rmw" 2 12 "$e2" python3 "$NODE" listener 12 > "$d/listener.log" 2>&1 &
    local lp=$!
    sleep 1
    run_in "$NS1" "$rmw" 1 9 "$e1" python3 "$NODE" talker 9 > "$d/talker.log" 2>&1
    wait "$lp"
    ran "$d/talker.log" 40 || { echo "-1"; return; }
    local got
    got=$(field "$d/listener.log" received)
    echo "${got:-0}"
}
t_range() {
    local iso sub
    iso=$(t_range_arm "$1" LOCALHOST "" ""); sub=$(t_range_arm "$1" SUBNET "" "")
    [ "$iso" = -1 ] || [ "$sub" = -1 ] && { echo "ERROR(talker did not run)"; return; }
    if [ "$iso" = 0 ] && [ "$sub" -ge 20 ]; then echo PASS; else echo "FAIL(localhost=$iso subnet=$sub)"; fi
}
t_peers() {
    # Its own control first: LOCALHOST without peers must isolate, or static peers would pass without doing anything
    # (found 2026-09-27: on rmw_tickle, which ignores the range, the first version of this test passed before any fix).
    local iso n
    iso=$(t_range_arm "$1" LOCALHOST "" "")
    n=$(t_range_arm "$1" LOCALHOST 10.77.0.2 10.77.0.1)
    [ "$iso" = -1 ] || [ "$n" = -1 ] && { echo "ERROR(talker did not run)"; return; }
    if [ "$iso" = 0 ] && [ "$n" -ge 20 ]; then echo PASS; else echo "FAIL(localhost=$iso localhost+static_peers=$n)"; fi
}
# The control per test: CycloneDDS, except where it does not produce the behaviour at all (set after checking it).
declare -A CONTROL=([itype]=${CONTROL_ITYPE:-rmw_cyclonedds_cpp})
ctl_fail=0
for t in $TESTS; do
    ctl=${CONTROL[$t]:-rmw_cyclonedds_cpp}
    for rmw in "$ctl" rmw_tickle; do
        setup_ns
        v=$("t_$t" "$rmw")
        VERDICT[$t:$rmw]=$v
        echo "$t $rmw | $v" | tee -a "$OUTDIR/summary.txt"
    done
    VERDICT[$t:control]="$ctl ${VERDICT[$t:$ctl]}"
    case "${VERDICT[$t:$ctl]}" in PASS) ;; *) ctl_fail=1; echo "$t: control ($ctl) failed - VOID" | tee -a "$OUTDIR/summary.txt" ;; esac
done
echo "=== results in $OUTDIR ===" | tee -a "$OUTDIR/summary.txt"
printf '%-9s | %-45s | %s\n' test control rmw_tickle | tee -a "$OUTDIR/summary.txt"
for t in $TESTS; do printf '%-9s | %-45s | %s\n' "$t" "${VERDICT[$t:control]}" "${VERDICT[$t:rmw_tickle]}"; done | tee -a "$OUTDIR/summary.txt"
exit "$ctl_fail"
