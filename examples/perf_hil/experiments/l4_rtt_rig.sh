#!/usr/bin/env bash
# L4 round trip on the rig: docs/RESULTS.md "rmw large messages (L4, cross-host)" measured as a round trip on ONE clock.
#
# Why: the L4 latency columns came from apex perf_test across two hosts, i.e. one-way time over two clocks (send stamp,
# or creation time minus a measured clock offset). The user decided (2026-10-19): (1) cross-host latency is measured by
# round trip only - the hosts' clocks drift and Ethernet PTP's error is large; (2) for a large message the reply is a
# small message. So: rmw_perf_pingpong's large_rtt_tx on rpi#1 publishes the cell's large message at the cell's rate,
# with QoS as the L4 cells; large_rtt_rx on rpi#2 answers every sample it takes with an 8-byte Ack (RELIABLE KEEP_LAST
# 10, the same rmw); the sender's round trip is reply arrival - the sample's due time, both on its own clock.
#
# Cells (CELLS; one name each; rates and QoS are L4's, the loss is tc netem on the sender's egress as L4's was):
#   CR    Ack (8 B)  RELIABLE KL10     30 Hz           the control for RELIABLE cells: a small ping, a small reply
#   I1    Array1m    RELIABLE KL10     30 Hz
#   I1L   Array1m    RELIABLE KL10     30 Hz, 5% loss
#   I1BE  Array1m    BEST_EFFORT KL5   30 Hz
#   I4    Array4m    RELIABLE KL10     15 Hz
#   I4BE  Array4m    BEST_EFFORT KL5   15 Hz
#   I4s   Array4m    BEST_EFFORT KL5   30 Hz           offers more than the link carries (L4's I4s)
#   CB    Ack (8 B)  BEST_EFFORT KL5   30 Hz           the control for BEST_EFFORT cells
# Arms (ARMS, L4's letters): tickle, fastdds (as shipped: eth0-only profile), fastdds@mms1472, cyclonedds. Every arm
# runs the same two binaries; RMW_IMPLEMENTATION (and its profile/URI) alone picks the rmw. Every rep runs every cell,
# and every cell every arm, in an order rotated per rep; the control cells open and close each rep.
#
# Samples per run: SAMPLES (default 900) scheduled samples, i.e. 30 s at 30 Hz and 60 s at 15 Hz. 900 is chosen from
# (1 - p)^n: a run's p99 needs P(no sample above the true p99) = 0.99^n < 0.001, n >= 688; 900 also leaves 30% for
# best-effort loss. 5 reps, as L4. The reading rules are in l4_rtt_read.py (--prereg prints them), written before any
# run; this script runs it at the end.
#
# Guards, enforced here:
#   - the rig lock (rig_lock.sh) is taken ONCE, by this process, for every cell (a queued campaign cannot cut in);
#   - SHA must be on origin/main (fetched here) - a commit nobody can rebuild is not measured;
#   - refuses to start if either Pi's eth0 carries a netem or tbf qdisc (someone's shaping is still on);
#   - one EXIT trap: removes every qdisc this script may have set on both Pis and stops any process it started (by
#     the PID it wrote itself at launch, and only if /proc/PID/exe is that binary);
#   - per run, identity: the RESULT/RX lines name the arm's rmw, and both processes' /proc/PID/maps show its library -
#     for rmw_tickle, exactly the one built here (AMENT_PREFIX_PATH shadowing has measured a stale binary before);
#     otherwise the run is VOID.
#
# USAGE (detached, from a durable checkout at a pushed SHA):
#   TS=$(date +%Y%m%d-%H%M%S); OUT=$HOME/rig_results_safe/l4_rtt_<sha8>_$TS
#   setsid nohup env SHA=<sha> OUT=$OUT examples/perf_hil/experiments/l4_rtt_rig.sh > $OUT.launch.log 2>&1 < /dev/null &
# DURATION: Pi builds ~15 min; per rep 4 arms x (6 cells x 30 s + 2 cells x 60 s + 8 x ~12 s overhead) = ~26 min
#   (the PC's overhead was 3.8 s a run; ssh and the Pis' slower start-up and discovery are allowed 12 s); 5 reps =
#   ~2 h 10 min; total ~2 h 30 min, analyse an overrun at 3 h 45 min.
#
# PC_PREFLIGHT=1 (no rig, no lock): "client" and "server" are two private network namespaces joined by a veth named
# eth0 in each (192.168.10.2 and .3, broadcast .255, a default route); every host command runs in its namespace as this
# user; nothing is built. PF_INSTALL is an install holding rmw_tickle and rmw_perf_pingpong (pc_preflight_build.sh's
# $D/install); /opt/ros/lyrical is the ROS. Fault arms: PF_NETEM_CLIENT / PF_NETEM_SERVER, netem words for that side's
# egress (e.g. "delay 1ms"), applied to every cell (the lossy cell's loss is appended on the client).
# PF_SHAPE: a qdisc for the client's egress in lossless cells instead (e.g. "tbf rate 1gbit burst 64kb latency 100ms";
# without it a veth carries I4s without saturating). PF_FDDS_DIR: the Fast DDS profiles to use on the PC (see below).
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
PC_PREFLIGHT=${PC_PREFLIGHT:-0}
export RIG_LOCK_SCOPE=hil
if [ "$PC_PREFLIGHT" != 1 ] && [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
# shellcheck source=examples/perf_hil/experiments/pi_checkout.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime
. "$X/pi_checkout.sh"
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime
. "$X/gate_v2_lib.sh"

K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
REPS=${REPS:-5}
SAMPLES=${SAMPLES:-900}
DRAIN=${DRAIN:-5}
CELLS=${CELLS:-"CR I1 I1L I1BE I4 I4BE I4s CB"}
ARMS=${ARMS:-"tickle fastdds fastdds@mms1472 cyclonedds"}
DOMAIN=${DOMAIN:-79}
PI_HOME=${PI_HOME:-/home/ci}
ROS_SETUP=${ROS_SETUP:-/opt/ros/jazzy/setup.bash}
PF_NETEM_CLIENT=${PF_NETEM_CLIENT:-}; PF_NETEM_SERVER=${PF_NETEM_SERVER:-}; PF_SHAPE=${PF_SHAPE:-}
case "$REPS$SAMPLES$DRAIN" in *[!0-9]*) echo "REPS, SAMPLES and DRAIN are whole numbers" >&2; exit 64 ;; esac
for v in "$PF_NETEM_CLIENT" "$PF_NETEM_SERVER" "$PF_SHAPE"; do
    case "$v" in *[!A-Za-z0-9.\ %]*) echo "PF_NETEM_*/PF_SHAPE may hold only [A-Za-z0-9. %]: '$v'" >&2; exit 64 ;; esac
done

cell_def() { # <cell> -> "msg qos depth rate loss"
    case "$1" in
        CR) echo "ack reliable 10 30 0" ;;
        CB) echo "ack best_effort 5 30 0" ;;
        I1) echo "array1m reliable 10 30 0" ;;
        I1L) echo "array1m reliable 10 30 5" ;;
        I1BE) echo "array1m best_effort 5 30 0" ;;
        I4) echo "array4m reliable 10 15 0" ;;
        I4BE) echo "array4m best_effort 5 15 0" ;;
        I4s) echo "array4m best_effort 5 30 0" ;;
        *) return 1 ;;
    esac
}
for c in $CELLS; do cell_def "$c" >/dev/null || { echo "unknown cell '$c'" >&2; exit 64; }; done
for a in $ARMS; do
    case "$a" in tickle | fastdds | fastdds@mms1472 | cyclonedds) ;; *) echo "unknown arm '$a'" >&2; exit 64 ;; esac
done

if [ "$PC_PREFLIGHT" = 1 ]; then
    : "${PF_INSTALL:?PC_PREFLIGHT needs PF_INSTALL, the install directory of pc_preflight_build.sh}"
    [ -x "$PF_INSTALL/rmw_perf_pingpong/lib/rmw_perf_pingpong/large_rtt_tx" ] ||
        { echo "no large_rtt_tx under $PF_INSTALL" >&2; exit 64; }
    CLIENT=l4c_$$; SERVER=l4s_$$
    SHA=${SHA:-$(git -C "$REPO" rev-parse HEAD)}
    ROS_SETUP=/opt/ros/lyrical/setup.bash
    # Both "hosts" share this PC's /tmp: one preflight at a time (a second would read the first one's pid files).
    PF_LOCK=/tmp/l4rtt_pc_preflight.$USER.lock
    exec 8>>"$PF_LOCK" || { echo "REFUSED: cannot open $PF_LOCK" >&2; exit 75; }
    flock -n 8 || { echo "REFUSED: another PC preflight holds $PF_LOCK" >&2; exit 75; }
    HOME_BIN=$PF_INSTALL/rmw_perf_pingpong/lib/rmw_perf_pingpong
    TICKLE_LIB=$PF_INSTALL/rmw_tickle/lib/librmw_tickle.so
    # lyrical's Fast DDS 3 matches nothing between the two namespaces with the profiles' interface allowlist (tried
    # 2026-10-19: no match in 30 s; without it, a match at once). A namespace holds only eth0 and lo, so the PC takes
    # PF_FDDS_DIR: the same two profiles without the allowlist (the mms1472 one keeping maxMessageSize 1472).
    FDDS_DIR=${PF_FDDS_DIR:-$REPO/examples/perf_hil/fastdds}
    BASE="set +u; source $ROS_SETUP; source $PF_INSTALL/local_setup.bash; set -u"
else
    [ -z "$PF_NETEM_CLIENT$PF_NETEM_SERVER$PF_SHAPE${PF_FDDS_DIR:-}" ] ||
        { echo "PF_* are for PC_PREFLIGHT=1 only" >&2; exit 64; }
    SHA=${SHA:?set SHA to a commit on origin/main}
    git -C "$REPO" fetch -q origin main || { echo "REFUSED: cannot fetch origin main" >&2; exit 1; }
    SHA=$(git -C "$REPO" rev-parse --verify --quiet "$SHA^{commit}") || { echo "REFUSED: unknown SHA" >&2; exit 1; }
    git -C "$REPO" merge-base --is-ancestor "$SHA" origin/main ||
        { echo "REFUSED: $SHA is not on origin/main" >&2; exit 1; }
    HOME_BIN=$PI_HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong
    TICKLE_LIB=$PI_HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so
    FDDS_DIR=$PI_HOME/tickle/examples/perf_hil/fastdds
    # Overlays with local_setup.bash only (a setup.bash re-sources its underlays and reorders the path).
    # shellcheck disable=SC2016 # expanded on the Pi
    BASE='set +u; source '$ROS_SETUP'; source $HOME/tickle/install/local_setup.bash; [ -f $HOME/rmw_perf_ws/install/local_setup.bash ] && source $HOME/rmw_perf_ws/install/local_setup.bash; set -u'
fi
OUT=${OUT:-$HOME/rig_results_safe/l4_rtt_${SHA:0:8}_$(date +%Y%m%d-%H%M%S)}
R=$OUT.runs
mkdir -p "$R" || exit 1
SUM=$OUT.txt
say() { echo "$*" | tee -a "$SUM"; }

sh_() {
    local h=$1; shift
    if [ "$PC_PREFLIGHT" = 1 ]; then
        sudo -n ip netns exec "$h" sudo -n -u "$USER" bash -c "$*"
    else
        ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "$@"
    fi
}
fetch_() { # host remote local
    if [ "$PC_PREFLIGHT" = 1 ]; then cp "$2" "$3" 2>/dev/null; else scp -q -i "$K" -o BatchMode=yes "ci@$1:$2" "$3" 2>/dev/null; fi
}
tc_() { # host tc-args...: as root (the Pis' sudoers permit tc; on the PC, ip netns exec runs it as root)
    local h=$1; shift
    if [ "$PC_PREFLIGHT" = 1 ]; then sudo -n ip netns exec "$h" tc "$@"; else sh_ "$h" "sudo -n tc $*" </dev/null; fi
}
qdisc_of() { # host: the root qdisc line of eth0
    if [ "$PC_PREFLIGHT" = 1 ]; then sudo -n ip netns exec "$1" tc qdisc show dev eth0; else sh_ "$1" "tc qdisc show dev eth0" </dev/null; fi
}

# shape <loss %>: the client's egress gets the cell's loss (and on the PC the fault-arm delay or PF_SHAPE), the server's
# the fault-arm delay; nothing otherwise.
shape() {
    local cw="$PF_NETEM_CLIENT"
    [ "$1" != 0 ] && cw="$cw loss $1%"
    tc_ "$CLIENT" qdisc del dev eth0 root >/dev/null 2>&1
    tc_ "$SERVER" qdisc del dev eth0 root >/dev/null 2>&1
    if [ -n "${cw// /}" ]; then
        # shellcheck disable=SC2086 # netem's words, checked above
        tc_ "$CLIENT" qdisc replace dev eth0 root netem $cw || return 1
    elif [ -n "$PF_SHAPE" ]; then
        # shellcheck disable=SC2086
        tc_ "$CLIENT" qdisc replace dev eth0 root $PF_SHAPE || return 1
    fi
    if [ -n "$PF_NETEM_SERVER" ]; then
        # shellcheck disable=SC2086
        tc_ "$SERVER" qdisc replace dev eth0 root netem $PF_NETEM_SERVER || return 1
    fi
    return 0
}

# stop_role <host> <role> <binary name>: SIGINT the process this script launched as <role>, by the PID its launcher
# wrote, only if /proc/PID/exe is that binary.
stop_role() {
    # shellcheck disable=SC2016 # expanded on the host
    sh_ "$1" 'f=/tmp/l4rtt_'"$2"'.pid; [ -s $f ] || exit 0; p=$(cat $f)
        case "$(readlink /proc/$p/exe 2>/dev/null)" in */'"$3"') kill -INT "$p" ;; esac' </dev/null >/dev/null 2>&1
}
alive_role() { # <host> <role> <binary>: 0 while that process runs
    # shellcheck disable=SC2016
    sh_ "$1" 'p=$(cat /tmp/l4rtt_'"$2"'.pid 2>/dev/null); [ -n "$p" ] && case "$(readlink /proc/$p/exe 2>/dev/null)" in */'"$3"') exit 0 ;; esac; exit 1' </dev/null
}

PC_VETH=l4$$
pc_netns_up() {
    local ns peer ip
    sudo -n ip netns add "$CLIENT" && sudo -n ip netns add "$SERVER" || return 1
    sudo -n ip link add "${PC_VETH}c" type veth peer name "${PC_VETH}s" || return 1
    for ns in "$CLIENT" "$SERVER"; do
        if [ "$ns" = "$CLIENT" ]; then peer=${PC_VETH}c ip=192.168.10.2; else peer=${PC_VETH}s ip=192.168.10.3; fi
        sudo -n ip link set "$peer" netns "$ns" && sudo -n ip -n "$ns" link set "$peer" name eth0 &&
            sudo -n ip -n "$ns" addr add "$ip/24" brd 192.168.10.255 dev eth0 && sudo -n ip -n "$ns" link set lo up &&
            sudo -n ip -n "$ns" link set eth0 up && sudo -n ip -n "$ns" route add default dev eth0 || return 1
    done
}

cleanup() { # the one EXIT trap
    for h in "$CLIENT" "$SERVER"; do
        tc_ "$h" qdisc del dev eth0 root >/dev/null 2>&1
    done
    stop_role "$CLIENT" tx large_rtt_tx
    stop_role "$SERVER" rx large_rtt_rx
    if [ "$PC_PREFLIGHT" = 1 ]; then
        sleep 2
        sudo -n ip netns del "$CLIENT" 2>/dev/null
        sudo -n ip netns del "$SERVER" 2>/dev/null
    fi
    echo "L4RTT_END $(date -Is) out=$OUT" >>"$SUM"
}
trap cleanup EXIT
if [ "$PC_PREFLIGHT" = 1 ]; then pc_netns_up || { say "PC preflight: cannot build the namespaces"; exit 1; }; fi

say "=== L4 round trip, $(date -Is): SHA $SHA, $REPS reps, $SAMPLES samples a run, drain $DRAIN s, cells: $CELLS," \
    "arms: $ARMS, pc preflight $PC_PREFLIGHT (client $CLIENT, server $SERVER), out $OUT ==="
[ "$PC_PREFLIGHT" = 1 ] && say "PC fault arms: client netem '$PF_NETEM_CLIENT', server netem '$PF_NETEM_SERVER', shape '$PF_SHAPE'"
printf 'sha=%s\nreps=%s\nsamples=%s\ndrain=%s\ncells=%s\narms=%s\npc_preflight=%s\npf_netem_client=%s\npf_netem_server=%s\npf_shape=%s\n' \
    "$SHA" "$REPS" "$SAMPLES" "$DRAIN" "$CELLS" "$ARMS" "$PC_PREFLIGHT" "$PF_NETEM_CLIENT" "$PF_NETEM_SERVER" \
    "$PF_SHAPE" >"$R/meta.txt"

# A shaped link at the start is someone's leftover: refuse rather than measure through it.
for h in "$CLIENT" "$SERVER"; do
    q=$(qdisc_of "$h") || { say "REFUSED: cannot read the qdisc of $h"; exit 1; }
    case "$q" in *netem* | *tbf*) say "REFUSED: $h eth0 is shaped ($q)"; exit 1 ;; esac
    say "qdisc $h: $(echo "$q" | head -1)"
done

if [ "$PC_PREFLIGHT" != 1 ]; then
    layout_cflags_default
    case "$TICKLE_EXTRA_CFLAGS" in *[!A-Za-z0-9=_\ -]*) say "TICKLE_EXTRA_CFLAGS may hold only [A-Za-z0-9=_ -]"; exit 64 ;; esac
    LAYOUT_BUILD="'-DCMAKE_C_FLAGS=$TICKLE_EXTRA_CFLAGS' '-DCMAKE_CXX_FLAGS=$TICKLE_EXTRA_CFLAGS'"
    RMW_TICKLE_BUILD="-DCMAKE_BUILD_TYPE=None '-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS' '-DCMAKE_CXX_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS'"
    say "--- building rmw_tickle + rmw_perf_pingpong at $SHA on both rpis (TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS') ---"
    pids=()
    for h in "$CLIENT" "$SERVER"; do
        # The three stages of rmw_crosshost_rtt.sh, in its order (builtin_interfaces/rcl_interfaces in rmw_perf_ws carry
        # TickLE's typesupport, so they are rebuilt against the typesupport just built, then rmw_perf_pingpong).
        sh_ "$h" "set -e
cd ~/tickle && $(pi_checkout "$SHA") && git clean -fdqx -e install -e build -e log
export PYTHONPATH=\$HOME/tickle/tools/typesupport\${PYTHONPATH:+:\$PYTHONPATH}
set +u; source $ROS_SETUP; set -u
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
  --cmake-args -DBUILD_SHARED_LIBS=ON $RMW_TICKLE_BUILD > /tmp/l4rtt_build1.log 2>&1 \
  || { echo \"STAGE 1 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/l4rtt_build1.log; exit 1; }
set +u; source \$HOME/tickle/install/setup.bash; set -u
colcon build --base-paths \$HOME/rmw_perf_ws --build-base \$HOME/rmw_perf_ws/build --install-base \$HOME/rmw_perf_ws/install \
  --packages-select builtin_interfaces rcl_interfaces --cmake-args -DCMAKE_BUILD_TYPE=Release $LAYOUT_BUILD --cmake-force-configure > /tmp/l4rtt_build2.log 2>&1 \
  || { echo \"STAGE 2 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/l4rtt_build2.log; exit 1; }
set +u; source \$HOME/rmw_perf_ws/install/setup.bash; source \$HOME/tickle/install/setup.bash; set -u
colcon build --packages-select rmw_perf_pingpong --cmake-args -DCMAKE_BUILD_TYPE=Release $LAYOUT_BUILD > /tmp/l4rtt_build3.log 2>&1 \
  || { echo \"STAGE 3 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/l4rtt_build3.log; exit 1; }
test -f $TICKLE_LIB && test -x $HOME_BIN/large_rtt_tx && test -x $HOME_BIN/large_rtt_rx
echo \"built on \$(hostname) at \$(git rev-parse --short HEAD)\"" </dev/null 2>&1 | tee -a "$SUM" &
        pids+=($!)
    done
    bad=0; for p in "${pids[@]}"; do wait "$p" || bad=1; done
    for h in "$CLIENT" "$SERVER"; do
        at=$(sh_ "$h" "git -C \$HOME/tickle rev-parse HEAD; test -x $HOME_BIN/large_rtt_rx && echo bin-ok" </dev/null)
        case "$at" in "$SHA"*bin-ok*) ;; *) bad=1; say "$h is not at $SHA with binaries" ;; esac
        say "librmw_tickle.so on $h: $(sh_ "$h" "sha256sum $TICKLE_LIB | cut -c1-16" </dev/null)"
    done
    [ "$bad" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }
fi

CDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General></Domain></CycloneDDS>'
env_for() {
    echo "$BASE"
    echo "unset RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS RMW_TICKLE_PUBLISH_HUGEPAGE TT_UDP_OFFLOAD TT_UDP_CSUM0 TT_RX_DIRECT"
    echo "unset FASTRTPS_DEFAULT_PROFILES_FILE FASTDDS_DEFAULT_PROFILES_FILE RMW_FASTRTPS_USE_QOS_FROM_XML RMW_FASTRTPS_PUBLICATION_MODE CYCLONEDDS_URI"
    case "$1" in
        tickle) echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
        fastdds) echo "export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTRTPS_DEFAULT_PROFILES_FILE=$FDDS_DIR/fastdds_eth0_only.xml" ;;
        fastdds@mms1472) echo "export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTRTPS_DEFAULT_PROFILES_FILE=$FDDS_DIR/fastdds_eth0_only_mms1472.xml" ;;
        cyclonedds) echo "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp CYCLONEDDS_URI='$CDDS_URI'" ;;
    esac
    echo "export ROS_DOMAIN_ID=$DOMAIN"
}
rmw_of() { case "$1" in tickle) echo rmw_tickle ;; fastdds*) echo rmw_fastrtps_cpp ;; cyclonedds) echo rmw_cyclonedds_cpp ;; esac; }

# The launcher: the process detached, its PID written by itself just before exec (never found by a pattern).
# shellcheck disable=SC2016 # expanded on the host
LAUNCHER='#!/bin/bash
role=$1; envfile=$2; shift 2
source "$envfile"
rm -f /tmp/l4rtt_$role.log /tmp/l4rtt_$role.pid /tmp/l4rtt_$role.maps /tmp/l4rtt_$role.samples
setsid nohup bash -c "echo \$\$ > /tmp/l4rtt_$role.pid; exec \"\$0\" \"\$@\"" "$@" > /tmp/l4rtt_$role.log 2>&1 < /dev/null &
for i in $(seq 1 50); do [ -s /tmp/l4rtt_$role.pid ] && break; sleep 0.1; done
cat /tmp/l4rtt_$role.pid'
for h in "$CLIENT" "$SERVER"; do
    printf '%s\n' "$LAUNCHER" | sh_ "$h" "cat > /tmp/l4rtt_launch.sh && chmod +x /tmp/l4rtt_launch.sh" || { say "launcher copy failed"; exit 1; }
    for a in $ARMS; do
        env_for "$a" | sh_ "$h" "cat > /tmp/l4rtt_env_${a/@/_}.sh" || { say "env copy failed"; exit 1; }
    done
done

# maps_ok <maps file> <arm>: the arm's rmw library is loaded, and for rmw_tickle exactly the one built here and no other.
maps_ok() {
    case "$2" in
        tickle) grep -qx "$TICKLE_LIB" "$1" && ! grep 'librmw_tickle\.so' "$1" | grep -vqx "$TICKLE_LIB" ;;
        fastdds*) grep -q '/librmw_fastrtps_cpp\.so' "$1" ;;
        cyclonedds) grep -q '/librmw_cyclonedds_cpp\.so' "$1" ;;
    esac
}

# read_maps <host> <role>: the rmw libraries in that process's /proc/PID/maps, polled until an rmw implementation is
# loaded (at rclcpp::init) or 15 s pass, into /tmp/l4rtt_<role>.maps.
read_maps() {
    # shellcheck disable=SC2016 # expanded on the host
    sh_ "$1" 'p=$(cat /tmp/l4rtt_'"$2"'.pid); for i in $(seq 1 75); do
        grep -o "/[^ ]*librmw_[a-z_]*\.so" /proc/$p/maps 2>/dev/null | sort -u > /tmp/l4rtt_'"$2"'.maps
        grep -qE "/librmw_(tickle|fastrtps_cpp|cyclonedds_cpp)\.so$" /tmp/l4rtt_'"$2"'.maps && break; sleep 0.2; done' </dev/null
}

run_one() { # rep cell arm
    local rep=$1 cell=$2 arm=$3 msg qos depth rate loss dur stem status="ok" t0 deadline envf
    read -r msg qos depth rate loss <<<"$(cell_def "$cell")"
    dur=$((SAMPLES / rate))
    stem="r${rep}_${cell}_${arm/@/_}"
    envf=/tmp/l4rtt_env_${arm/@/_}.sh
    shape "$loss" || { say "RUN $stem: cannot shape the link"; exit 1; }
    sh_ "$SERVER" "/tmp/l4rtt_launch.sh rx $envf $HOME_BIN/large_rtt_rx -m $msg --qos $qos --depth $depth" </dev/null >/dev/null
    read_maps "$SERVER" rx
    sleep 1
    sh_ "$CLIENT" "/tmp/l4rtt_launch.sh tx $envf $HOME_BIN/large_rtt_tx -m $msg --rate $rate -d $dur --qos $qos --depth $depth --drain $DRAIN --samples /tmp/l4rtt_tx.samples" </dev/null >/dev/null
    read_maps "$CLIENT" tx
    t0=$(date +%s); deadline=$((t0 + dur + 30 + 20 + DRAIN + 30))
    while alive_role "$CLIENT" tx large_rtt_tx; do
        if [ "$(date +%s)" -ge "$deadline" ]; then status="VOID:timeout"; stop_role "$CLIENT" tx large_rtt_tx; sleep 2; break; fi
        sleep 1
    done
    stop_role "$SERVER" rx large_rtt_rx
    for _ in 1 2 3 4 5 6 7 8 9 10; do alive_role "$SERVER" rx large_rtt_rx || break; sleep 0.5; done
    fetch_ "$CLIENT" /tmp/l4rtt_tx.log "$R/${stem}_tx.log"
    fetch_ "$CLIENT" /tmp/l4rtt_tx.samples "$R/${stem}_tx.samples"
    fetch_ "$CLIENT" /tmp/l4rtt_tx.maps "$R/${stem}_tx.maps"
    fetch_ "$SERVER" /tmp/l4rtt_rx.log "$R/${stem}_rx.log"
    fetch_ "$SERVER" /tmp/l4rtt_rx.maps "$R/${stem}_rx.maps"
    local want; want=$(rmw_of "$arm")
    if [ "$status" = ok ] && ! grep -q "^RESULT: " "$R/${stem}_tx.log" 2>/dev/null &&
        grep -q "^no warm-up round trip" "$R/${stem}_tx.log" 2>/dev/null; then
        # Matched, but not one sample answered in 20 s: the framework does not deliver in this cell (FastDDS as shipped
        # at 5% loss, on the PC). Not VOID - a run that cannot deliver is the finding - if its identity holds.
        if grep -q "^RX: framework=$want " "$R/${stem}_rx.log" 2>/dev/null && maps_ok "$R/${stem}_tx.maps" "$arm" &&
            maps_ok "$R/${stem}_rx.maps" "$arm"; then
            status=nodelivery
        else
            status="VOID:nodelivery_identity"
        fi
    elif [ "$status" = ok ]; then
        if ! grep -q "^RESULT: framework=$want " "$R/${stem}_tx.log" 2>/dev/null; then status="VOID:no_result_or_wrong_rmw"
        elif ! grep -q "^RX: framework=$want " "$R/${stem}_rx.log" 2>/dev/null; then status="VOID:no_rx_line_or_wrong_rmw"
        elif ! maps_ok "$R/${stem}_tx.maps" "$arm" || ! maps_ok "$R/${stem}_rx.maps" "$arm"; then status="VOID:maps"
        elif [ ! -s "$R/${stem}_tx.samples" ]; then status="VOID:no_samples"; fi
    fi
    echo "RUN rep=$rep cell=$cell arm=$arm stem=$stem status=$status msg=$msg qos=$qos depth=$depth rate=$rate loss=$loss dur=$dur" >>"$R/index.txt"
    say "RUN $stem $status | $(grep -h '^RESULT:' "$R/${stem}_tx.log" 2>/dev/null | grep -oE ' (sent=[0-9]+ replied=[0-9]+|rtt_p50_ms=[0-9.-]+|rtt_p99_ms=[0-9.-]+)' | tr '\n' ' ')"
}

read -r -a ARMS_A <<<"$ARMS"; NA=${#ARMS_A[@]}
for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep/$REPS $(date -Is) ---"
    for cell in $CELLS; do
        for i in $(seq 0 $((NA - 1))); do
            run_one "$rep" "$cell" "${ARMS_A[$(((i + rep - 1) % NA))]}"
        done
    done
done
python3 "$X/l4_rtt_read.py" "$OUT" >"$OUT.verdicts.txt" 2>&1
rc=$?
tee -a "$SUM" <"$OUT.verdicts.txt"
say "L4RTT_DONE reader_rc=$rc $(date -Is)"
