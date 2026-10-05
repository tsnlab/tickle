#!/usr/bin/env bash
# rmw_tickle RELIABLE + KEEP_ALL across the rig's link, before and after the ACK-solicitation fix (d7e02846,
# 8d1c3712), with rmw_fastrtps_cpp and rmw_cyclonedds_cpp as controls the fix cannot touch.
#
# Why: the fix made a KEEP_ALL publisher that is refused (window full of unacknowledged samples) solicit an ACK at
# once and re-ask by itself, where before it could wait out a retry interval. Under rmw_tickle a refused publish
# blocks for up to max_blocking_time, so the stall, if the rmw layer sees it, shows as delivered rate at max rate,
# most under loss. Every published rmw KEEP_ALL figure predates the fix. ROADMAP "Now" item 5.
#
# Shape: apex performance_test (`perf_test -c ROS2`), publisher alone on the client Pi (-s 0), subscriber alone on the
# server Pi (-p 0), --reliable, history KEEP_ALL (the default when --keep_last is omitted), -r 0 (as fast as
# possible), for DUR seconds. tc netem loss on the client's egress. Every arm runs the same perf_test binary;
# RMW_IMPLEMENTATION and, for rmw_tickle, which librmw_tickle.so is first on the path select the arm.
#
# Arms:
#   tickle@pre   674f0dcb (parent of d7e02846: no fix)
#   tickle@ack   8d1c3712 (exactly the two ACK-solicitation commits on top of pre)
#   tickle@head  $HEAD_SHA (ack plus the later core commits: per-sleep doorbell, size-limit walk - both on the
#                shared-memory path, which two hosts never take)
#   fastdds, cyclonedds  - controls: no TickLE code; their rep-to-rep spread is the rig's drift floor.
# Interleaved: every rep runs all five arms, in an order rotated per rep.
#
# HOW TO READ IT, written before running and enforced in the summary below:
#   VOID run: perf_test's /proc/PID/maps on either Pi does not show the arm's own rmw library (for tickle, the
#     variant's exact path), the subscriber reported no data rows, or the subscriber received nothing.
#   VOID overall: the three tickle .so files are not pairwise different (the arms would be one binary). A cell is
#     VOID when neither control has 2 usable reps. (Changed after the first run, before any rmw_tickle data: it said
#     "either control", and FastDDS refusing every 5%-loss rep would have voided every lossy cell for good.)
#   REFUSED run (added after the first run, before any rmw_tickle data existed): the publisher ended on
#     "failed to publish" - a write that blocked past max_blocking_time throws out of rclcpp and ends perf_test.
#     That is the KEEP_ALL contract refusing, not a broken run: it is reported with the seconds it survived and
#     left out of the rate comparison. FastDDS did this in every 5%-loss run. A vendor publisher that ended before
#     its maps were read is identified by its log's "RMW Implementation:" line; a tickle arm always needs its maps.
#   CORRECTNESS, must hold for every tickle@ack and tickle@head run: lost == 0 and the publisher exited cleanly
#     (no "terminate"/"exception"/"timeout" in its log). A failure here is a defect, whatever the rates say.
#     tickle@pre is reported, not judged: it is the build being replaced.
#   RATE (delivered msgs/s at the subscriber, seconds 3..DUR): per cell, ack vs pre and head vs ack.
#     BETTER   if the mean difference exceeds 2 x its standard error AND the ratio exceeds the larger of the two
#              controls' own max/min rep ratio in that cell (a change smaller than the rig's drift is not ours);
#     WORSE    symmetric;  otherwise HELD.
#   Falsification: if ack is not BETTER than pre in any 5%-loss cell, the fix does not reach the rmw layer at max
#     rate on this link; that is a valid result and is published as such.
#   Latency is printed but not judged: the two Pis' clocks are not synchronised to the precision it would need.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi

K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
HEAD_SHA=${HEAD_SHA:?set HEAD_SHA to a pushed commit}
PRE_SHA=${PRE_SHA:-674f0dcb}; ACK_SHA=${ACK_SHA:-8d1c3712}
REPS=${REPS:-3}; DUR=${DUR:-20}; TOPICS=${TOPICS:-"Array1k Array4k"}; LOSSES=${LOSSES:-"0 5"}
ARMS=${ARMS:-"tickle@pre tickle@ack tickle@head fastdds cyclonedds"}
DOMAIN=${DOMAIN:-61}
OUT=${OUT:-$HOME/rig_results_safe/rmw_keepall_rig_$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT.runs"
SUM="$OUT.txt"
sh_() { local h=$1; shift; ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "$@"; }
say() { echo "$*" | tee -a "$SUM"; }

set_loss() {
    if [ "$1" = 0 ]; then sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" </dev/null >/dev/null 2>&1 || true
    else sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss $1%" </dev/null; fi
}
cleanup() {
    set_loss 0
    for h in "$CLIENT" "$SERVER"; do
        # shellcheck disable=SC2016 # expanded on the Pi, not here
        sh_ "$h" 'for f in /tmp/ka_*.pid; do [ -f "$f" ] || continue; p=$(cat "$f");
            case "$(readlink /proc/$p/exe 2>/dev/null)" in */perf_test) kill -INT "$p";; esac; rm -f "$f"; done' \
            </dev/null >/dev/null 2>&1
    done
}
trap cleanup EXIT

say "=== rmw KEEP_ALL on the rig, $(date -Is): head $HEAD_SHA, ack $ACK_SHA, pre $PRE_SHA, $REPS reps, ${DUR}s," \
    "topics: $TOPICS, loss: $LOSSES, arms: $ARMS, out $OUT ==="

# ---- build: head into ~/tickle/install (typesupport too), pre and ack as librmw_tickle.so-only variants ----------
if [ "${SKIP_BUILD:-0}" != 1 ]; then
say "--- building on both Pis ---"
pids=()
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $HEAD_SHA && git clean -fdqx -e install -e build -e log
export PYTHONPATH=\$HOME/tickle/tools/typesupport\${PYTHONPATH:+:\$PYTHONPATH}
set +u; source /opt/ros/jazzy/setup.bash; set -u
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
  --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release > /tmp/ka_build1.log 2>&1 \
  || { echo \"STAGE 1 FAILED on \$(hostname)\"; tail -25 /tmp/ka_build1.log; exit 1; }
set +u; source \$HOME/tickle/install/setup.bash; set -u
# The interface packages' TickLE bindings are regenerated with this generator: perf_test's were from 2026-09-20.
colcon build --base-paths \$HOME/rmw_perf_ws --build-base \$HOME/rmw_perf_ws/build --install-base \$HOME/rmw_perf_ws/install \
  --packages-select builtin_interfaces rcl_interfaces performance_test --cmake-args -DCMAKE_BUILD_TYPE=Release \
  --cmake-force-configure > /tmp/ka_build2.log 2>&1 \
  || { echo \"STAGE 2 FAILED on \$(hostname)\"; tail -25 /tmp/ka_build2.log; exit 1; }
rm -rf \$HOME/rmw_variants; git worktree prune
for v in pre:$PRE_SHA ack:$ACK_SHA; do
  name=\${v%%:*}; sha=\${v#*:}
  git worktree add -q --detach \$HOME/rmw_variants/\$name/src \$sha
  colcon build --base-paths \$HOME/rmw_variants/\$name/src --build-base \$HOME/rmw_variants/\$name/build \
    --install-base \$HOME/rmw_variants/\$name/install --packages-select rmw_tickle \
    --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release > /tmp/ka_build_\$name.log 2>&1 \
    || { echo \"VARIANT \$name FAILED on \$(hostname)\"; tail -25 /tmp/ka_build_\$name.log; exit 1; }
done
echo \"built on \$(hostname): head \$(git rev-parse --short HEAD), pre \$(git -C \$HOME/rmw_variants/pre/src rev-parse --short HEAD), ack \$(git -C \$HOME/rmw_variants/ack/src rev-parse --short HEAD)\"
sha256sum \$HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so \$HOME/rmw_variants/*/install/rmw_tickle/lib/librmw_tickle.so" \
        </dev/null 2>&1 | tee -a "$SUM" &
    pids+=($!)
done
bad=0; for p in "${pids[@]}"; do wait "$p" || bad=1; done
[ "$bad" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }
fi

# The three tickle binaries must differ, on each Pi, or the arms are one binary under three names.
for h in "$CLIENT" "$SERVER"; do
    n=$(sh_ "$h" "sha256sum \$HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so \$HOME/rmw_variants/pre/install/rmw_tickle/lib/librmw_tickle.so \$HOME/rmw_variants/ack/install/rmw_tickle/lib/librmw_tickle.so | awk '{print \$1}' | sort -u | wc -l" </dev/null)
    if [ "$n" != 3 ]; then say "VOID OVERALL: $h has $n distinct librmw_tickle.so, not 3"; exit 1; fi
done
say "identity: three distinct librmw_tickle.so on both Pis"

CDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General></Domain></CycloneDDS>'
FDDS_PROFILE=/home/ci/tickle/examples/perf_hil/fastdds/fastdds_eth0_only.xml
# Overlays are sourced with local_setup.bash only: a setup.bash re-sources its build-time underlays, which moves
# /opt/ros/jazzy back in front of rmw_perf_ws and loads interface packages without TickLE typesupport.
# shellcheck disable=SC2016 # expanded on the Pi, not here
BASE='set +u; source /opt/ros/jazzy/setup.bash; source $HOME/tickle/install/local_setup.bash
source $HOME/rmw_perf_ws/install/local_setup.bash'
env_for() {
    echo "$BASE"
    case "$1" in
        tickle@head) echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
        tickle@*) echo "source \$HOME/rmw_variants/${1#tickle@}/install/local_setup.bash"
            echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
        fastdds) echo "export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTRTPS_DEFAULT_PROFILES_FILE=$FDDS_PROFILE" ;;
        cyclonedds) echo "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp CYCLONEDDS_URI='$CDDS_URI'" ;;
    esac
    echo "export ROS_DOMAIN_ID=$DOMAIN"
}
lib_for() {
    case "$1" in
        tickle@head) echo "/home/ci/tickle/install/rmw_tickle/lib/librmw_tickle.so" ;;
        tickle@*) echo "/home/ci/rmw_variants/${1#tickle@}/install/rmw_tickle/lib/librmw_tickle.so" ;;
        fastdds) echo "librmw_fastrtps_cpp.so" ;;
        cyclonedds) echo "librmw_cyclonedds_cpp.so" ;;
    esac
}

# The launcher on each Pi: perf_test detached, its PID written by itself just before exec (so it is perf_test's own,
# never found by a pattern), and 2 s later (before a refused write can end it) the rmw libraries its /proc/PID/maps shows.
# shellcheck disable=SC2016 # expanded on the Pi, not here
LAUNCHER='#!/bin/bash
role=$1; envfile=$2; shift 2
source "$envfile"
PT=$HOME/rmw_perf_ws/install/performance_test/lib/performance_test/perf_test
rm -f /tmp/ka_$role.log /tmp/ka_$role.maps /tmp/ka_$role.pid
setsid nohup bash -c "echo \$\$ > /tmp/ka_$role.pid; exec \"\$0\" \"\$@\"" "$PT" "$@" > /tmp/ka_$role.log 2>&1 < /dev/null &
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s /tmp/ka_$role.pid ] && break; sleep 0.2; done
p=$(cat /tmp/ka_$role.pid)
( sleep 2; grep -o "/[^ ]*librmw_[a-z_]*\.so" /proc/$p/maps 2>/dev/null | sort -u > /tmp/ka_$role.maps ) > /dev/null 2>&1 < /dev/null &
'
for h in "$CLIENT" "$SERVER"; do
    printf '%s' "$LAUNCHER" | sh_ "$h" "cat > /tmp/ka_launch.sh && chmod +x /tmp/ka_launch.sh" || { say "launcher copy failed"; exit 1; }
    for arm in $ARMS; do
        env_for "$arm" | sh_ "$h" "cat > /tmp/ka_env_${arm/@/_}.sh" || { say "env copy failed"; exit 1; }
    done
done

launch() { # host role arm args...
    local h=$1 role=$2 arm=$3; shift 3
    sh_ "$h" "/tmp/ka_launch.sh $role /tmp/ka_env_${arm/@/_}.sh $*" </dev/null
}
wait_done() { # $1 host, $2 role, $3 deadline seconds
    sh_ "$1" "p=\$(cat /tmp/ka_$2.pid); for i in \$(seq 1 $3); do [ -d /proc/\$p ] || exit 0; sleep 1; done;
        case \"\$(readlink /proc/\$p/exe)\" in */perf_test) kill -INT \$p;; esac; sleep 2; echo killed" </dev/null
}

run_one() { # arm topic loss rep
    local arm=$1 topic=$2 loss=$3 rep=$4 stem="$1_$2_l$3_r$4"
    local common="-c ROS2 -t $topic --reliable --dds_domain_id $DOMAIN"
    # Every arm, not only rmw_tickle's: rclcpp's type-description service needs a typesupport rmw_tickle does not
    # provide (its first run here died on it), and switching off an introspection service changes no data path.
    local rosargs="--ros-args --param start_type_description_service:=false"
    launch "$SERVER" sub "$arm" "$common -p 0 -s 1 --expected_num_pubs 1 --max_runtime $((DUR + 8)) $rosargs"
    sleep 2
    launch "$CLIENT" pub "$arm" "$common -r 0 -p 1 -s 0 --expected_num_subs 1 --max_runtime $DUR $rosargs"
    local k1 k2
    k1=$(wait_done "$CLIENT" pub $((DUR + 40)))
    k2=$(wait_done "$SERVER" sub 20)
    scp -q -i "$K" -o BatchMode=yes "ci@$CLIENT:/tmp/ka_pub.log" "$OUT.runs/${stem}_pub.log" 2>/dev/null
    scp -q -i "$K" -o BatchMode=yes "ci@$SERVER:/tmp/ka_sub.log" "$OUT.runs/${stem}_sub.log" 2>/dev/null
    local mp ms
    mp=$(sh_ "$CLIENT" "cat /tmp/ka_pub.maps 2>/dev/null" </dev/null | tr '\n' ' ')
    ms=$(sh_ "$SERVER" "cat /tmp/ka_sub.maps 2>/dev/null" </dev/null | tr '\n' ' ')
    echo "RUN $stem pub_maps=[$mp] sub_maps=[$ms] pub_killed=${k1:-no} sub_killed=${k2:-no} want=$(lib_for "$arm")" \
        >> "$OUT.runs/index.txt"
}

for loss in $LOSSES; do
    set_loss "$loss" || { say "tc failed for $loss%"; exit 1; }
    say "--- loss $loss% ($(sh_ "$CLIENT" "tc qdisc show dev eth0" </dev/null | head -1)) ---"
    for topic in $TOPICS; do
        for rep in $(seq 1 "$REPS"); do
            read -r -a order <<<"$ARMS"
            n=${#order[@]}; s=$(( (rep - 1) % n ))
            for i in $(seq 0 $((n - 1))); do
                arm=${order[$(( (i + s) % n ))]}
                run_one "$arm" "$topic" "$loss" "$rep"
                say "  $(date +%T) $arm $topic l$loss r$rep done"
            done
        done
    done
done
set_loss 0
say "=== runs done $(date -Is); tc restored ==="

python3 "$REPO/examples/perf_hil/experiments/rmw_keepall_rig_summary.py" "$OUT.runs" "$DUR" | tee -a "$SUM"
