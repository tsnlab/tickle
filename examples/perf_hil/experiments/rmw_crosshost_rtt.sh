#!/usr/bin/env bash
# Cross-host rmw-layer round trip: rmw_tickle against rmw_fastrtps_cpp and rmw_cyclonedds_cpp, each
# reached the way a ROS 2 application reaches it (rclcpp, RMW_IMPLEMENTATION), ping on rpi#1 and pong
# on rpi#2 (rmw_tickle/rmw_perf_pingpong). Re-measures COMPARISON.MD's rmw rows, the only rows where
# TickLE is not first, on the current core. The previous figures (0.524/0.523 ms against FastDDS
# 0.516/0.523 and CycloneDDS 0.440/0.449, 2026-09-21) predate the release build of both core and
# rmw_tickle, the scheduler-driven poll, sendmmsg and DATA_FRAG.
#
# Fairness, enforced rather than assumed (COMPARISON.MD 4.4):
#   - all three on the eth0 test link only: TICKLE_BROADCAST_ADDR for rmw_tickle, the eth0-only XML
#     profile for FastDDS (without it FastDDS also sends on wlan0 - the 2026-09-22 E2 finding),
#     CYCLONEDDS_URI for CycloneDDS, the same strings the native harnesses use;
#   - the same nodes, the same QoS (depth 8, best_effort or reliable), the same pinning (cores 1-3),
#     the same ROS_DOMAIN_ID, interleaved by rmw within every repetition;
#   - IDENTITY per row: the ping's RESULT line names the rmw that actually loaded (framework=), and
#     the pong's /proc/PID/maps must show the expected librmw_*.so - for rmw_tickle, the one this run
#     just built under ~/tickle/install, since a stale copy elsewhere on AMENT_PREFIX_PATH has
#     measured the wrong binary before. The pong's PID comes from its own launch ($!), never from a
#     name pattern.
#
# HOW TO READ IT, written before running:
#   - rmw_tickle's round trip should fall from 0.52 ms: its core is ~0.21 ms natively and both layers
#     are now release builds. Whether it falls below CycloneDDS is the question this run answers, not
#     an expectation; if it does not, the gap is in rmw_tickle's own layer (the native core already
#     wins the same round trip), and that is where the next optimisation looks.
#   - reliable and best_effort should read within noise of each other for every rmw, as before.
#   - Any row with loss, a missing RESULT, or a failed identity check is VOID, not averaged in.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
SHA=${SHA:?set SHA to the commit to build and measure}
REPS=${REPS:-3}
MSGS=${MSGS:-"bench array1k"}
DOMAIN=${DOMAIN:-73}
OUT=${OUT:-/tmp/rmw_crosshost_rtt_$(date +%Y%m%d-%H%M%S).txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== rmw cross-host RTT, $(date -Is), SHA $SHA, $REPS reps, msgs: $MSGS ==="

CDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>'
FDDS_PROFILE=/home/ci/tickle/examples/perf_hil/fastdds/fastdds_eth0_only.xml
ENV_BASE="set +u; source /opt/ros/jazzy/setup.bash; [ -f \$HOME/rmw_perf_ws/install/setup.bash ] && source \$HOME/rmw_perf_ws/install/setup.bash; source \$HOME/tickle/install/setup.bash; set -u; export ROS_DOMAIN_ID=$DOMAIN"
env_for() {
    case "$1" in
        rmw_tickle) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
        rmw_fastrtps_cpp) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTRTPS_DEFAULT_PROFILES_FILE=$FDDS_PROFILE" ;;
        rmw_cyclonedds_cpp) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp CYCLONEDDS_URI='$CDDS_URI'" ;;
    esac
}
lib_for() {
    case "$1" in
        rmw_tickle) echo "/home/ci/tickle/install/rmw_tickle/lib/librmw_tickle.so" ;;
        rmw_fastrtps_cpp) echo "/librmw_fastrtps_cpp.so" ;;
        rmw_cyclonedds_cpp) echo "/librmw_cyclonedds_cpp.so" ;;
    esac
}

say "--- building rmw_tickle + rmw_perf_pingpong (Release) at $SHA on both rpis ---"
pids=()
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
set +u; source /opt/ros/jazzy/setup.bash; [ -f \$HOME/rmw_perf_ws/install/setup.bash ] && source \$HOME/rmw_perf_ws/install/setup.bash; set -u
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle rmw_perf_pingpong \
  --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release > /tmp/rmwx_build.log 2>&1 \
  || { echo \"BUILD FAILED on \$(hostname)\"; tail -25 /tmp/rmwx_build.log; exit 1; }
test -f \$HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so
test -x \$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong/ping_node
test -x \$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong/pong_node
echo \"built on \$(hostname) at \$(git rev-parse --short HEAD)\"" 2>&1 | tee -a "$OUT" &
    pids+=($!)
done
bad=0; for p in "${pids[@]}"; do wait "$p" || bad=1; done
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "test -x \$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong/pong_node" || bad=1
done
[ "$bad" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }

BIN=/home/ci/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong
one() { # $1 rmw, $2 msg, $3 qos (best_effort|reliable), $4 rep
    local rmw="$1" msg="$2" qos="$3" rep="$4" flag="" env pongpid maps res verdict=ok
    [ "$qos" = reliable ] && flag="--reliable"
    env=$(env_for "$rmw")
    pongpid=$(sh_ "$SERVER" "$env; nohup taskset -c 1-3 $BIN/pong_node $flag -m $msg > /tmp/rmwx_pong.log 2>&1 < /dev/null & echo \$!")
    sleep 4
    maps=$(sh_ "$SERVER" "grep -o '/[^ ]*librmw_[a-z_]*\.so' /proc/$pongpid/maps 2>/dev/null | sort -u | tr '\n' ' '" || true)
    res=$(sh_ "$CLIENT" "$env; timeout 60 taskset -c 1-3 $BIN/ping_node -i 0.1 -d 10 $flag -m $msg 2>/dev/null" | grep '^RESULT:' || true)
    sh_ "$SERVER" "[ -d /proc/$pongpid ] && [ \"\$(readlink /proc/$pongpid/exe)\" = $BIN/pong_node ] && kill -INT $pongpid" >/dev/null 2>&1 || true
    sleep 1
    case "$maps" in *"$(lib_for "$rmw")"*) ;; *) verdict="VOID(pong loaded: ${maps:-nothing})" ;; esac
    case "$res" in *"framework=$rmw "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(no RESULT for $rmw)" ;; esac
    case "$res" in *"loss_pct=0 "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(loss)" ;; esac
    say "$rmw $msg $qos rep$rep | $verdict | ${res#RESULT: }"
}
for rep in $(seq 1 "$REPS"); do
    for msg in $MSGS; do
        for qos in best_effort reliable; do
            for rmw in rmw_tickle rmw_fastrtps_cpp rmw_cyclonedds_cpp; do
                one "$rmw" "$msg" "$qos" "$rep"
            done
        done
    done
done
say ""; say "=== done ==="
