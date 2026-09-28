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
#   bagstall ros2 bag record while the recorder is SIGSTOPped behind a KEEP_LAST depth-10 talker (g13, the bound on
#            zero loss: a KEEP_ALL reader never destroys an unread sample but cannot make the writer keep one)
#   events   a listener on rclpy's EventsExecutor receives the other side's talker                 (g2, on-new-data callbacks)
#   matched  PUBLICATION_MATCHED / SUBSCRIPTION_MATCHED fire on both sides                          (g3)
#   itype    PUBLISHER_ / SUBSCRIPTION_INCOMPATIBLE_TYPE fire for String vs Int32 on one topic     (g3)
#   takeseq  the rmw library defines rmw_take_sequence (a symbol check, as rcl_take_sequence dispatches to it) (g5)
#   samehost talker and listener as two processes on ONE host, and `ros2 topic echo` next to a talker (g8)
#   durable  a late TRANSIENT_LOCAL KEEP_LAST 4 subscriber on the other host gets exactly the last 4 of 6 60,000-byte
#            samples, in order (g10)
#   inprocess a talker node and a listener node in ONE process, and a service call between them    (g9)
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
TESTS=${*:-graph bag bagstall events matched itype takeseq samehost inprocess durable range peers introspect}
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
        # local_setup.bash for the overlay: its setup.bash re-sources the rmw install it was built against, which can
        # shadow WS/rmw (found by Dev on 2026-09-27: a whole run measured an older rmw_tickle that way).
        rmw_tickle) e="$e; source $WS/rmw/install/setup.bash; source $WS/ifaces/install/local_setup.bash; export TICKLE_BROADCAST_ADDR=10.77.0.255" ;;
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
    run_in "$NS1" "$rmw" 1 14 "" python3 "$NODE" listener 14 dump > "$d/listener.log" 2>&1 &
    local lp=$!
    sleep 4
    run_in "$NS2" "$rmw" 2 10 "" ros2 bag play "$d/bag" > "$d/play.log" 2>&1
    wait "$lp"
    local got gaps
    got=$(field "$d/listener.log" received)
    # Content, not only count (LARGE_MESSAGE_PLAN pass 4): the replayed numbers must run in order without a gap, i.e.
    # the recorded bytes deserialised to the same strings, in the order they were recorded.
    gaps=$(grep -h '^SEQ:' "$d/listener.log" | tail -1 | tr ' ' '\n' | grep -E '^[0-9]+$' |
        awk 'NR > 1 && $1 != prev + 1 { g++ } { prev = $1 } END { print g + 0 }')
    if [ "${recorded:-0}" -ge 30 ] && [ "${got:-0}" -ge 30 ] && [ "${gaps:-1}" = 0 ]; then echo PASS
    else echo "FAIL(recorded=${recorded:-0} replayed=${got:-0} out_of_order_or_missing=${gaps:-none})"; fi
}
# bagstall: a recorder that stops taking, behind an ordinary KEEP_LAST depth-10 publisher (g13
# criterion 5's stall arm, RMW_GAPS_PLAN.md). The base `bag` test shows a recorder that keeps up
# loses nothing; this one asks what happens when it does not, which is the case the zero-loss claim
# cannot cover: a KEEP_ALL reader never destroys an unread sample, but it cannot make the WRITER
# keep one, and a KEEP_LAST depth-10 writer evicts rather than blocking.
#
# The stall is SIGSTOP on the recorder, which is the honest model of "the application stopped
# taking" and is identical for both rmws - no knob of ours is involved, so the arms are comparable.
# Loss is (published - recorded) on both sides, taken from the talker's own RESULT line and from
# `ros2 bag info`, i.e. from rosbag2 rather than from any counter of ours, so neither arm is
# measured by an instrument the other does not have.
#
# Pre-registered reading:
#   both lose         - this is DDS semantics rather than a shortfall, and the criterion is that
#                       rmw_tickle's loss does not exceed the control's, and that ours is visible
#                       (gap_evicted non-zero in the recorder's own shutdown line).
#   only rmw_tickle   - a real gap with its own number; the writer's retention or the reader's
#                       recovery is worse than CycloneDDS's and that has to be decided, not excused.
#   neither loses     - the stall did not stall. VOID, not a pass: check the recorder was actually
#                       stopped and that enough was published while it was.
t_bagstall() {
    local rmw=$1 d=$OUTDIR/bagstall_$1
    mkdir -p "$d"
    run_in "$NS1" "$rmw" 1 26 "" python3 "$NODE" talker 26 > "$d/talker.log" 2>&1 &
    local tp=$!
    sleep 3

    # The recorder's PID is written by the shell that becomes it (exec), so it is captured at launch
    # and never searched for - `pgrep` here would match this script's own command line as readily as
    # the recorder (the project's rule 2, learned the hard way).
    local pidfile="$d/recorder.pid"
    run_in "$NS2" "$rmw" 2 20 "" bash -c "'echo \$\$ > $pidfile; exec ros2 bag record -o $d/bag --topics /accept_chatter'" \
        > "$d/record.log" 2>&1 &
    local rp=$!
    sleep 5

    local pid="" stalled=0 why="pid not written"
    pid=$(cat "$pidfile" 2>/dev/null)
    # Verified before it is signalled, not assumed from the file: a stale pidfile from an earlier run
    # would otherwise aim SIGSTOP at whatever holds that number now. Note the empty-pid guard is not
    # tidiness - with an empty $pid the path below becomes /proc//cmdline, which reads the KERNEL
    # command line and succeeds, so the check would answer about something else entirely.
    #
    # Each way this can fail says so separately. The first version of this printed one message for
    # all three, and when it fired the run could not say whether the recorder was missing, the
    # verification had failed, or the signal had been refused - which is the same defect as a test
    # that cannot say what it failed on.
    # The empty-pid branch comes first and returns without touching /proc, rather than relying on the
    # next test to be false: with an empty $pid the path is /proc//cmdline, which is the KERNEL
    # command line, and it exists and reads successfully - so the verification would answer about
    # something else entirely. Refusing before the read is cheaper than a comment explaining it.
    if [ -z "$pid" ]; then
        why="pid not written"
    elif ! tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q 'bag record'; then
        why="pid $pid is not the recorder: [$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | head -c 120)]"
    elif ! kill -STOP "$pid" 2>/dev/null; then
        # The recorder runs as this same user (run_in's own `sudo -u $USER`), so this needs no sudo -
        # and the earlier version's `sudo -n kill` failed silently when sudo wanted a password.
        why="SIGSTOP refused for pid $pid"
    else
        stalled=1
        sleep 8 # ~80 samples published at 10 Hz while nobody is taking; the writer retains 10
        kill -CONT "$pid" 2>/dev/null
    fi
    wait "$rp" 2>/dev/null
    wait "$tp"

    if [ "$stalled" != 1 ]; then echo "ERROR(recorder not stalled: $why)"; return; fi
    ran "$d/talker.log" 50 || { echo "ERROR(talker did not run)"; return; }

    local published recorded evicted lost
    published=$(field "$d/talker.log" sent)
    recorded=$(run_in "$NS2" "$rmw" 2 10 "" ros2 bag info "$d/bag" 2>/dev/null |
        grep -oE 'Messages: +[0-9]+' | grep -oE '[0-9]+' | head -1)
    lost=$((${published:-0} - ${recorded:-0}))
    # Ours only, and reported beside the loss rather than instead of it: the incomplete-delivery rule
    # turns on whether a drop is visible, and this is where ours becomes visible.
    evicted=$(grep -hoE 'gap_evicted=[0-9]+' "$d/record.log" 2>/dev/null | tail -1 | cut -d= -f2)
    local numbers="published=${published:-0} recorded=${recorded:-0} lost=$lost gap_evicted=${evicted:-n/a}"
    # No loss means the stall did not stall - VOID, not a pass. Written this way round deliberately:
    # "nobody lost anything" is what a broken stall and a perfect reader both look like, and only one
    # of them is a result.
    if [ "$lost" -le 0 ]; then echo "ERROR(stall produced no loss, so it did not stall: $numbers)"; return; fi
    # Ours must additionally be visible. gap_evicted counts the samples the WRITER said it no longer
    # held, which is a subset of the loss - the rest went while the reader was stopped and its window
    # moved past them - so this is "the loss is counted", not "the counter equals the loss", and the
    # two numbers are printed side by side rather than one standing for the other.
    if [ "$rmw" = rmw_tickle ] && [ "${evicted:-0}" -le 0 ]; then
        echo "FAIL(loss is not visible in any counter: $numbers)"
        return
    fi
    echo "PASS($numbers)"
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
t_samehost() {
    # Both processes in NS1 - one host, one address (2026-09-28: rmw_tickle received 0 here, CycloneDDS 80).
    local rmw=$1 d=$OUTDIR/samehost_$1
    mkdir -p "$d"
    run_in "$NS1" "$rmw" 1 12 "" python3 "$NODE" listener 12 > "$d/listener.log" 2>&1 &
    local lp=$!
    sleep 1
    run_in "$NS1" "$rmw" 1 14 "" python3 "$NODE" talker 14 > "$d/talker.log" 2>&1 &
    local tp=$!
    sleep 4
    run_in "$NS1" "$rmw" 1 8 "" timeout 8 ros2 topic echo --once /accept_chatter std_msgs/msg/String > "$d/echo.log" 2>&1
    wait "$lp"; wait "$tp"
    ran "$d/talker.log" 50 || { echo "ERROR(talker did not run)"; return; }
    local got echo_ok=0
    got=$(field "$d/listener.log" received)
    grep -q 'data: msg-' "$d/echo.log" && echo_ok=1
    if [ "${got:-0}" -ge 50 ] && [ "$echo_ok" = 1 ]; then echo PASS; else echo "FAIL(listener=${got:-0} echo=$echo_ok)"; fi
}
t_inprocess() {
    local rmw=$1 d=$OUTDIR/inprocess_$1
    mkdir -p "$d"
    run_in "$NS1" "$rmw" 1 12 "" python3 "$NODE" inprocess 12 > "$d/inprocess.log" 2>&1
    ran "$d/inprocess.log" 50 || { echo "ERROR(the process did not run)"; return; }
    local got svc
    got=$(field "$d/inprocess.log" received); svc=$(field "$d/inprocess.log" service_ok)
    if [ "${got:-0}" -ge 50 ] && [ "${svc:-0}" = 1 ]; then echo PASS; else echo "FAIL(received=${got:-0} service_ok=${svc:-0})"; fi
}
t_durable() {
    local rmw=$1 d=$OUTDIR/durable_$1
    mkdir -p "$d"
    run_in "$NS1" "$rmw" 1 14 "" python3 "$NODE" durable_pub 14 > "$d/pub.log" 2>&1 &
    local pp=$!
    sleep 5
    run_in "$NS2" "$rmw" 2 7 "" python3 "$NODE" durable_sub 7 > "$d/sub.log" 2>&1
    wait "$pp"
    ran "$d/pub.log" 6 || { echo "ERROR(publisher did not run)"; return; }
    local got first last order
    got=$(field "$d/sub.log" received); first=$(field "$d/sub.log" first); last=$(field "$d/sub.log" last)
    order=$(field "$d/sub.log" in_order)
    if [ "${got:-0}" = 4 ] && [ "${first:-0}" = 3 ] && [ "${last:-0}" = 6 ] && [ "${order:-0}" = 1 ]; then echo PASS
    else echo "FAIL(received=${got:-0} first=${first:-0} last=${last:-0} in_order=${order:-0})"; fi
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
# g14's generalisation: every value an introspection API reports is handed back to the API that would consume it -
# the type name to the typesupport lookup, the name and QoS to create_publisher(), the GID to the uniqueness a tool
# matching samples to writers depends on. g14 itself was found the hard way, by rosbag2 refusing to replay a recording
# whose QoS we had supplied; this asks the same question directly instead of waiting for a tool to ask it.
#
# PASS means the node reported at least one endpoint and every round trip succeeded. A control that fails voids the
# case, as everywhere else here: if CycloneDDS cannot round-trip its own report, the test is wrong, not the rmw.
t_introspect() {
    local d=$OUTDIR/introspect_$1
    mkdir -p "$d"
    # Two processes, because the endpoint has to be a REMOTE one. Dev showed on 2026-09-29 that the first version of
    # this case - one node reading its own publisher back - passes with g14's fix reverted, because a local endpoint is
    # served from the real profile it was created with and never went through rmw_qos_profile_unknown. A case that
    # passes with the defect present is not a case.
    run_in "$NS2" "$1" 2 12 "" python3 "$NODE" intropeer 10 > "$d/peer.log" 2>&1 &
    local pp=$!
    sleep 1
    run_in "$NS1" "$1" 1 9 "" python3 "$NODE" introspect 8 > "$d/node.log" 2>&1
    wait "$pp"
    # Not ran(): that helper reads a sent= field, which this role does not print, so it could only ever fail - a check
    # that cannot pass, which is how both arms including the control reported ERROR on the first run. The node's own
    # RESULT line is the evidence it finished.
    grep -q 'RESULT: role=introspect' "$d/node.log" || { echo "ERROR(node did not finish)"; return; }
    local n f
    n=$(field "$d/node.log" discovered); f=$(field "$d/node.log" roundtrip_failures)
    # discovered=0 is not a pass: it means the case found nothing to check, which must not read as success.
    if [ "${n:-0}" -lt 1 ]; then echo "FAIL(no remote endpoint discovered)"; return; fi
    # Dev's PASS(detail) generalisation of the control check has landed (see the case below), so the
    # count could be carried here now.
    # (was: a bare PASS on purpose until it landed - the count is in
    # the node's own RESULT line either way, and a PASS(...) the control check does not yet accept would void the case
    # for a control failure that did not happen.
    if [ "${f:-1}" = 0 ]; then
        echo PASS
    else
        # Not field(): that extracts numeric values only, so a text detail came back empty and the
        # failure printed as a bare FAIL() - silent precisely when it had something to say.
        echo "FAIL($(result "$d/node.log" | grep -oE 'detail=[^ ]+' | cut -d= -f2-))"
    fi
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
    # OFF (added 2026-09-28, after g6): nothing between processes at all, CycloneDDS's observed behaviour; in-process
    # delivery is the inprocess test's business.
    local iso sub off
    iso=$(t_range_arm "$1" LOCALHOST "" ""); sub=$(t_range_arm "$1" SUBNET "" ""); off=$(t_range_arm "$1" OFF "" "")
    { [ "$iso" = -1 ] || [ "$sub" = -1 ] || [ "$off" = -1 ]; } && { echo "ERROR(talker did not run)"; return; }
    if [ "$iso" = 0 ] && [ "$sub" -ge 20 ] && [ "$off" = 0 ]; then echo PASS; else echo "FAIL(localhost=$iso subnet=$sub off=$off)"; fi
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
# The rmw_tickle every test loads must be WS's: resolve it the way the test processes will, and refuse otherwise.
prefix=$(bash -c "set +u; $(env_for rmw_tickle 1 ""); set -u; ros2 pkg prefix rmw_tickle" 2>/dev/null)
if [ "$prefix" != "$WS/rmw/install/rmw_tickle" ]; then
    echo "ERROR: rmw_tickle resolves to '${prefix:-nothing}', not $WS/rmw/install/rmw_tickle - a shadowed workspace" | tee -a "$OUTDIR/summary.txt"
    exit 3
fi
echo "rmw_tickle from $prefix ($(md5sum "$prefix/lib/librmw_tickle.so" | cut -c1-12))" | tee -a "$OUTDIR/summary.txt"
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
    # PASS or PASS(detail): a test whose result is a measurement rather than a yes/no carries its
    # numbers in the verdict (bagstall), and throwing them away to satisfy an exact match would lose
    # the only thing that run produced. Every test that prints a bare PASS still matches.
    case "${VERDICT[$t:$ctl]}" in PASS | PASS\(*) ;; *) ctl_fail=1; echo "$t: control ($ctl) failed - VOID" | tee -a "$OUTDIR/summary.txt" ;; esac
done
echo "=== results in $OUTDIR ===" | tee -a "$OUTDIR/summary.txt"
printf '%-9s | %-45s | %s\n' test control rmw_tickle | tee -a "$OUTDIR/summary.txt"
for t in $TESTS; do printf '%-9s | %-45s | %s\n' "$t" "${VERDICT[$t:control]}" "${VERDICT[$t:rmw_tickle]}"; done | tee -a "$OUTDIR/summary.txt"
exit "$ctl_fail"
