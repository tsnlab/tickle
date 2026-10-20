#!/usr/bin/env bash
# rmw_wake_split.sh - where an rmw ping-pong's receive time goes, framework-neutral, on the PC (ROADMAP "Other open
# items": rmw kernel wake ~6 us and poll-to-executor handoff ~21 us; 2026-10-20). PC-only, veth between two private
# netns, so no rig lock and nothing reaches the 10.1.1.x LAN.
#
# One run: pong_node in NS2, ping_node --wait block in NS1, both with --stamps. On one host the two processes share
# CLOCK_MONOTONIC, so their stamps split every round trip into
#   fwd   ping send_ns -> pong callback_ns   (ping publish, kernel tx, veth, pong wake, rmw receive, executor)
#   turn  pong callback_ns -> publish_return  (the reply's publish)
#   back  pong publish_return -> ping reply_ns (ping wake, rmw receive, executor; the veth softirq runs in the
#                                               pong's sendmsg, so it is inside turn, not back)
# for any rmw, vendors included. An rmw_tickle built with -DRMW_TICKLE_TRACE=ON also dumps its own stamps
# (RMW_TICKLE_TRACE_FILE), which rmw_wake_split.py splits further, by thread.
#
# Identity: for rmw_tickle the pong's /proc/PID/maps must name $INSTALL's librmw_tickle.so, read while it runs
# (PID from the launching shell's own $$ before exec, not a search); a mismatch voids the run. For a vendor, the maps
# must name its librmw_<rmw>.so.
#
# SYSSTAMP=<libsysstamp.so> (optional): both nodes under sysstamp.c, for rmw_wake_split.py's kernel segments. For
# rmw_tickle it needs a build with -DCMAKE_C_FLAGS=-DTT_HAL_LIBC_CALLS, or its raw system calls bypass the layer.
# RMW_ENV="KEY=VALUE ..." (optional): exported for both nodes, e.g. RMW_TICKLE_EXECUTOR_HOLDOVER=0 for an arm of
# one build. Both nodes' CPU: the pong's threads' schedstat read just before it is stopped (P.pong_sched.txt), the
# ping's task-clock from perf stat (P.ping_cpu.txt).
# PIN_PING / PIN_PONG="<cpu list>" (optional): taskset -c for that node, every thread of it. Unpinned, one arm's
# per-run RTT medians on this PC (a 16-vCPU guest) spread 39-58 us at a 5 ms gap - the placement, not the arm.
# Usage: INSTALL=<colcon install dir> RMW=rmw_tickle|rmw_cyclonedds_cpp|rmw_fastrtps_cpp GAP=0.005 DUR=5
#        MSG=bench QOS=best_effort|reliable OUTDIR=<dir> TAG=<name> rmw_wake_split.sh
# Results: $OUTDIR/$TAG.{ping,pong}.stamps, .{ping,pong}.trace (tickle traced builds), .{ping,pong}.log,
#          .pong_sched.txt, and one line "RUN tag=... ok|VOID ..." on stdout.
set -u
INSTALL=${INSTALL:?INSTALL=<install dir>}
RMW=${RMW:-rmw_tickle}
GAP=${GAP:-0.005}
DUR=${DUR:-5}
MSG=${MSG:-bench}
QOS=${QOS:-best_effort}
OUTDIR=${OUTDIR:?OUTDIR=<dir>}
TAG=${TAG:?TAG=<name>}
BIN="$INSTALL/rmw_perf_pingpong/lib/rmw_perf_pingpong"
mkdir -p "$OUTDIR"
O="$OUTDIR/$TAG"
NS1=ws-ns1-$$
NS2=ws-ns2-$$
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    rm -f "$O.done"
    return 0
}
trap cleanup EXIT
trap 'exit 130' INT TERM
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add ws1 netns "$NS1" type veth peer name ws2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev ws1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev ws2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set ws1 up
sudo -n ip -n "$NS2" link set ws2 up
# A default route over the veth: Fast DDS's multicast discovery has no route without one.
sudo -n ip -n "$NS1" route add default dev ws1
sudo -n ip -n "$NS2" route add default dev ws2

pre_pong="" pre_ping=""
if [ -n "${SYSSTAMP:-}" ]; then # sysstamp.c's LD_PRELOAD layer: every socket and wait call, any rmw
    pre_pong="export LD_PRELOAD=$SYSSTAMP SYSSTAMP_FILE=$O.sys.pong;"
    pre_ping="export LD_PRELOAD=$SYSSTAMP SYSSTAMP_FILE=$O.sys.ping;"
fi
reliable=""
[ "$QOS" = reliable ] && reliable="--reliable"
rm -f "$O".* 2>/dev/null
env=". /opt/ros/lyrical/setup.bash; . $INSTALL/setup.bash; export ${RMW_ENV:-} RMW_IMPLEMENTATION=$RMW TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_HOME=$OUTDIR/roshome ROS_LOG_DIR=$OUTDIR/roslog FASTDDS_BUILTIN_TRANSPORTS=UDPv4 LD_LIBRARY_PATH=$INSTALL/rmw_tickle/lib:\$LD_LIBRARY_PATH"
asme() { # <ns> <command>: run as this user inside the netns
    sudo -n ip netns exec "$1" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups env HOME="$HOME" bash -c "$2"
}
# The pong writes its own PID, then execs; schedstat is read by its shell's child just before it is stopped.
asme "$NS2" "$env; export RMW_TICKLE_TRACE_FILE=$O.pong.trace; echo \$\$ > $O.pong.pid; $pre_pong exec ${PIN_PONG:+taskset -c $PIN_PONG} $BIN/pong_node -m $MSG $reliable --stamps $O.pong.stamps" >"$O.pong.log" 2>&1 &
sleep 3
pong_pid=$(cat "$O.pong.pid" 2>/dev/null || echo 0)
want="$INSTALL/rmw_tickle/lib/librmw_tickle.so"
[ "$RMW" != rmw_tickle ] && want="/lib${RMW}.so"
loaded=$(grep -m1 -o -E "/[^ ]*/librmw_(tickle|cyclonedds_cpp|fastrtps_cpp|zenoh_cpp)\.so" "/proc/$pong_pid/maps" 2>/dev/null || true)
asme "$NS1" "$env; export RMW_TICKLE_TRACE_FILE=$O.ping.trace; $pre_ping perf stat -x, -e task-clock -o $O.ping_cpu.txt -- timeout -s INT $((DUR + 10)) ${PIN_PING:+taskset -c $PIN_PING} $BIN/ping_node -d $DUR -i $GAP --wait block -m $MSG $reliable --stamps $O.ping.stamps" >"$O.ping.log" 2>&1
cat "/proc/$pong_pid/task/"*/schedstat >"$O.pong_sched.txt" 2>/dev/null
# Read the maps again at the end too: the library must have been the same one throughout.
loaded_end=$(grep -m1 -o -E "/[^ ]*/librmw_(tickle|cyclonedds_cpp|fastrtps_cpp|zenoh_cpp)\.so" "/proc/$pong_pid/maps" 2>/dev/null || true)
[ -r "/proc/$pong_pid/exe" ] && [ "$(readlink "/proc/$pong_pid/exe")" = "$BIN/pong_node" ] && kill -INT "$pong_pid"
for _ in $(seq 1 50); do [ -e "/proc/$pong_pid" ] || break; sleep 0.1; done
status=ok
case "$loaded" in *"$want") ;; *) status="VOID(loaded=$loaded want=$want)" ;; esac
[ "$loaded" = "$loaded_end" ] || status="VOID(library changed: $loaded -> $loaded_end)"
grep -q '^RESULT' "$O.ping.log" || status="VOID(no RESULT line)"
echo "RUN tag=$TAG rmw=$RMW env=[${RMW_ENV:-}] pin=[${PIN_PING:-}/${PIN_PONG:-}] qos=$QOS msg=$MSG gap=$GAP dur=$DUR $status loaded=$loaded $(grep -h -o 'sent=[0-9]* recv=[0-9]*' "$O.ping.log" | head -1)"
