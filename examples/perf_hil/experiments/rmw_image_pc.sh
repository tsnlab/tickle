#!/usr/bin/env bash
# rmw_image_pc.sh - large-message stage 2, step A (docs/DESIGN.md section 8): sensor_msgs/Image of 1 MB and 4 MB
# through rmw_tickle between two private network namespaces on this PC, RELIABLE KEEP_LAST 10 at 0% and 5% netem
# loss on the publisher's egress, and sensor_data at 0% (rmw_image_node.py at both ends, rclpy).
#
# rmw_tickle is built from this tree with the shared-memory transport compiled out (-Dtt_SEGMENT_ENABLED=0): the
# namespaces share /dev/shm, so with it the two would be same-host peers - step B's path, not A's. sensor_msgs and
# what it nests are regenerated with this tree's typesupport (build_ros2_interfaces.sh), whose direct codec now
# carries a sequence of 65,535 elements or more; the interfaces of any other workspace were generated without it.
#
# NOT a rig figure: one PC, a veth pair, Python at both ends. HOW TO READ IT, written before the first run
# (2026-10-09): RELIABLE at 0% and 5% - delivered = published, torn = 0, out_of_order = 0; sensor_data at 0% -
# torn = 0 (a best-effort sample lost on the way is a lost sample, never a torn one). CONTROL: 1 MB RELIABLE at 0%
# must deliver every sample, or the setup is broken and the other rows say nothing.
#
# Usage: rmw_image_pc.sh [REPS] [DURATION_S]    results to $OUT (default ~/largemsg_pc/rmw_image_results.txt)
set -u
REPS=${1:-2}
DURATION=${2:-15}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_pc}
WS=$WORK/ws
OUT=${OUT:-$WORK/rmw_image_results.txt}
NS1=lgimg-pub-$$
NS2=lgimg-sub-$$
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
mkdir -p "$WS"
ROS_SETUP=$(find /opt/ros -maxdepth 2 -name setup.bash -print -quit)
set +u
# shellcheck disable=SC1090
source "$ROS_SETUP"
set -u
if [ "${SKIP_BUILD:-0}" != 1 ]; then
    colcon --log-base "$WS/log" build --base-paths "$TREE/rmw_tickle" \
        --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
        --build-base "$WS/rmw/build" --install-base "$WS/rmw/install" \
        --cmake-args -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_C_FLAGS=-Dtt_SEGMENT_ENABLED=0" \
        > "$WORK/build_rmw.log" 2>&1 || { echo "rmw build failed: $WORK/build_rmw.log"; exit 1; }
    set +u
    # shellcheck disable=SC1091
    source "$WS/rmw/install/setup.bash"
    set -u
    "$TREE/rmw_tickle/scripts/build_ros2_interfaces.sh" -w "$WS/ifaces" sensor_msgs > "$WORK/build_ifaces.log" 2>&1 ||
        { echo "interface build failed: $WORK/build_ifaces.log"; exit 1; }
fi
# The interfaces this does not rebuild (rcl_interfaces, ...) from an existing TickLE interface workspace, if any.
UNDERLAY=${UNDERLAY:-$HOME/rmw_accept_dev/ifaces/install/local_setup.bash}
ENV_SETUP="source $ROS_SETUP; source $WS/rmw/install/local_setup.bash"
[ -f "$UNDERLAY" ] && ENV_SETUP="$ENV_SETUP; source $UNDERLAY"
ENV_SETUP="$ENV_SETUP; source $WS/ifaces/install/local_setup.bash"

cleanup
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add lgimg1 netns "$NS1" type veth peer name lgimg2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev lgimg1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev lgimg2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set lgimg1 up
sudo -n ip -n "$NS2" link set lgimg2 up

sha=$(git -C "$TREE" rev-parse --short HEAD)
echo "=== rmw_image_pc $(date -Is) tree=$sha reps=$REPS duration=${DURATION}s ===" | tee -a "$OUT"
run_as_me() { # ns, command: the node runs as this user inside the namespace, with the ROS environment
    sudo -n ip netns exec "$1" sudo -n -u "$USER" bash -c "$ENV_SETUP; export RMW_IMPLEMENTATION=rmw_tickle \
        TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_DOMAIN_ID=73; $2"
}
cell() { # name width height rate loss flags
    local name=$1 w=$2 h=$3 rate=$4 loss=$5 flags=$6 rep
    sudo -n ip netns exec "$NS1" tc qdisc del dev lgimg1 root 2>/dev/null
    [ "$loss" != 0 ] && { sudo -n ip netns exec "$NS1" tc qdisc add dev lgimg1 root netem loss "${loss}%" || return 1; }
    for rep in $(seq 1 "$REPS"); do
        run_as_me "$NS2" "python3 $HERE/rmw_image_node.py sub --width $w --height $h --duration $((DURATION + 12)) $flags" \
            > "$WORK/img_sub.log" 2>&1 &
        local sub_pid=$!
        sleep 3
        run_as_me "$NS1" "python3 $HERE/rmw_image_node.py pub --width $w --height $h --rate $rate --duration $DURATION $flags" \
            > "$WORK/img_pub.log" 2>&1
        wait "$sub_pid"
        local p s
        p=$(grep -m1 '^RESULT:' "$WORK/img_pub.log" | sed 's/^RESULT: //')
        s=$(grep -m1 '^RESULT:' "$WORK/img_sub.log" | sed 's/^RESULT: //')
        if [ -z "$p" ] || [ -z "$s" ]; then
            echo "$name loss=${loss}% rep$rep VOID (pub: $(tail -1 "$WORK/img_pub.log") / sub: $(tail -1 "$WORK/img_sub.log"))" | tee -a "$OUT"
            continue
        fi
        echo "$name loss=${loss}% rep$rep | $p | $s" | tee -a "$OUT"
    done
}
# CELLS="name:loss ..." runs only those, e.g. CELLS="RELIABLE_4MB:5".
want() { [ -z "${CELLS:-}" ] || case " $CELLS " in *" $1:$2 "*) return 0 ;; *) return 1 ;; esac; }
want RELIABLE_1MB 0 && cell RELIABLE_1MB 512 512 30 0 ""
want RELIABLE_1MB 5 && cell RELIABLE_1MB 512 512 30 5 ""
want SENSOR_1MB 0 && cell SENSOR_1MB 512 512 30 0 "--best-effort"
want RELIABLE_4MB 0 && cell RELIABLE_4MB 1024 1024 15 0 ""
want RELIABLE_4MB 5 && cell RELIABLE_4MB 1024 1024 15 5 ""
echo "=== done $(date -Is) ===" | tee -a "$OUT"
