#!/usr/bin/env bash
# Large-message step B on the rig, same host, with a control perf_test cannot swamp (docs/ROADMAP.md "Wired work";
# docs/DESIGN.md section 8 "Step B"; the pre-registration B2 is at `git show 228da18d:docs/DESIGN.md`).
#
# Why a new harness. B2 ran on perf_test (rig_queue_largemsg_B.sh). Its control, Array16k same-host, went VOID on the
# subscriber's CPU per MB (+5%, 5 of 5 reps) and was shown to be perf_test's own AnalyzeRunner: once a second it
# busy-waits on the DataRunner's SpinLock until the next sample, ~33 ms of CPU a second against ~1.4 ms/s of rmw work,
# and one more period whenever it loses the race (bcf8624d). The re-run on the same day read the control HELD only
# because that spin had widened its 2 x SE to 4.2 ms/MB on a +3.3 ms/MB difference: a control that holds by being
# noisy decides nothing. So the instrument changes, not the threshold:
#
#   - large_rtt_tx / large_rtt_rx (rmw_perf_pingpong, the L4 round trip) on ONE Pi: the sender sleeps to each due time,
#     both executors block in the rmw's wait, and each process reads its own CLOCK_PROCESS_CPUTIME_ID at the edges of
#     the scheduled samples (cpu_window_ms) - no thread of the benchmark spins, and discovery, warm-up and shutdown are
#     outside the window. Latency is the round trip on the sender's clock (large sample out, 8-byte Ack back).
#   - Step B is on main since 6e6429bc, and RMW_TICKLE_LARGE_REGION=0 turns it off at run time. So tickle@A (off) and
#     tickle@B (on) are ONE build, one library, one pair of binaries: the arms differ by one environment variable.
#   - The control the change provably cannot touch: the Ack cells (8 B ping, 8 B reply). rmw_tickle_large_region_options()
#     returns for a type that always fits a datagram BEFORE it reads RMW_TICKLE_LARGE_REGION, so in those cells tickle@A
#     and tickle@B execute the same code with the same inputs - an A/A by construction, and checked per run: no tickle
#     process in a control cell maps a /dev/shm/tickle-large-* region.
#   - The instrument's own spread at the large sizes: tickle@A2, a second label of tickle@A (same everything). Its
#     difference from tickle@A in each large cell says what this instrument can resolve at that size.
#
# Cells, all on the client Pi, 900 scheduled samples a run (30 s at 30 Hz). CONTROLS, arms CONTROL_ARMS only
# (tickle@A tickle@B), open AND close every rep (slots 1 and 2), so the control brackets the rep in time:
#   CR    Ack      RELIABLE KEEP_LAST 10   30 Hz   control, RELIABLE
#   CB    Ack      BEST_EFFORT KEEP_LAST 5 30 Hz   control, BEST_EFFORT
# CELLS, every arm in ARMS, between them:
#   S1    Array1m  RELIABLE KEEP_LAST 10   30 Hz   B2's RELIABLE 1 MB cell
#   S1BE  Array1m  BEST_EFFORT KEEP_LAST 5 30 Hz   B2's BEST_EFFORT 1 MB cell
#   S4    Array4m  RELIABLE KEEP_LAST 10   30 Hz   B2's RELIABLE 4 MB cell (same host: 30 Hz, the user 2026-10-09)
#   S4BE  Array4m  BEST_EFFORT KEEP_LAST 5 30 Hz   B2's BEST_EFFORT 4 MB cell
# Arms (ARMS): tickle@A (RMW_TICKLE_LARGE_REGION=0: step A, fragments through the ring), tickle@A2 (the same again),
# tickle@B (the default: step B), fastdds and cyclonedds at their shipped same-host defaults (no profile, no URI; Fast
# DDS's data-sharing/SHM default), as B2. The arm order is rotated per rep. tickle arms are compared PAIRED, run against
# run within a rep and slot: the PC dry run showed drift common to every arm from one rep to the next (+-10%) that an
# unpaired 2 x SE on 3 reps cannot separate from a bias. The reading rules are stepb_rtt_read.py's (--prereg prints them); it runs at the end, and its --selftest must
# print SELFTEST PASS before anything starts.
#
# Guards, enforced here:
#   - SHA must be on origin/main (fetched here) and carry both RMW_TICKLE_LARGE_REGION (rmw_large.c) and the CPU window
#     (large_rtt_rx.cpp); otherwise REFUSED before the lock;
#   - the rig lock (rig_lock.sh, scope hil) is taken ONCE, by this process, for the whole job; RIG_LOCK_WAIT 36000 s;
#   - refuses to start if the client Pi's eth0 or lo carries a netem or tbf qdisc (someone's shaping is still on); this
#     harness sets no qdisc;
#   - one EXIT trap: stops what it launched (by the PID its launcher wrote, only if /proc/PID/exe is that binary) and
#     prints STEPB_RTT_END;
#   - per run, identity: RESULT/RX lines name the arm's rmw and both processes' /proc/PID/maps show its library (for
#     rmw_tickle exactly the one built here); and the treatment: both processes' maps read twice during the schedule
#     (+8 s and +16 s after the sender starts, every arm alike) for /dev/shm/tickle-large-*. The reader voids a run
#     whose treatment is not its arm's.
#
# USAGE (detached, from a durable checkout at a pushed SHA; the reader and this script are taken from that checkout):
#   TS=$(date +%Y%m%d-%H%M%S); OUT=$HOME/rig_results_safe/stepb_rtt_<sha8>_$TS
#   setsid nohup env SHA=<sha> OUT=$OUT examples/perf_hil/experiments/stepb_rtt_rig.sh > $OUT.launch.log 2>&1 < /dev/null &
# The job's last line is always "STEPB_RTT_END ..." (the trap), after "STEPB_RTT_DONE reader_rc=<n>" on completion.
# ESTIMATE: build on one Pi ~15 min; a run 30 s + ~14 s (launch, match, warm-up, drain, two ssh maps reads, fetch;
#   the L4 harness allows 12 s for two hosts; the PC took ~3 s) = ~44 s; a rep 2 x (2 controls x 2 arms) + 4 cells x 5
#   arms = 28 runs = ~21 min; 5 reps ~1 h 43 min; total ~2 h. Analyse an overrun at 3 h.
#
# PC_PREFLIGHT=1 (no rig, no lock, nothing built): the client is a private network namespace whose eth0 is one end of a
# veth (192.168.77.2/24, broadcast .255, a default route; the other end sits in a second, empty namespace); both
# processes run in it as this user. PF_INSTALL: an install holding rmw_tickle and rmw_perf_pingpong at the commit being
# checked (pc_preflight_build.sh's $D/install). /opt/ros/lyrical is the ROS.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
PC_PREFLIGHT=${PC_PREFLIGHT:-0}
REPS=${REPS:-5}
SAMPLES=${SAMPLES:-900}
DRAIN=${DRAIN:-5}
CELLS=${CELLS:-"S1 S1BE S4 S4BE"}
ARMS=${ARMS:-"tickle@A tickle@A2 tickle@B fastdds cyclonedds"}
CONTROLS=${CONTROLS:-"CR CB"}
CONTROL_ARMS=${CONTROL_ARMS:-"tickle@A tickle@B"}
DOMAIN=${DOMAIN:-81}
PI_HOME=${PI_HOME:-/home/ci}
ROS_SETUP=${ROS_SETUP:-/opt/ros/jazzy/setup.bash}
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; BCAST=192.168.10.255
case "$REPS$SAMPLES$DRAIN$DOMAIN" in *[!0-9]*) echo "REPS, SAMPLES, DRAIN and DOMAIN are whole numbers" >&2; exit 64 ;; esac

cell_def() { # <cell> -> "msg qos depth rate bytes"
    case "$1" in
        CR) echo "ack reliable 10 30 8" ;;
        CB) echo "ack best_effort 5 30 8" ;;
        S1) echo "array1m reliable 10 30 1048576" ;;
        S1BE) echo "array1m best_effort 5 30 1048576" ;;
        S4) echo "array4m reliable 10 30 4194304" ;;
        S4BE) echo "array4m best_effort 5 30 4194304" ;;
        *) return 1 ;;
    esac
}
for c in $CELLS $CONTROLS; do cell_def "$c" >/dev/null || { echo "unknown cell '$c'" >&2; exit 64; }; done
for a in $ARMS $CONTROL_ARMS; do
    case "$a" in tickle@A | tickle@A2 | tickle@B | fastdds | cyclonedds) ;; *) echo "unknown arm '$a'" >&2; exit 64 ;; esac
done

# The reader decides every verdict; one that cannot pass its own selftest is not trusted with a rig session.
python3 "$X/stepb_rtt_read.py" --selftest 2>&1 | tail -1 | grep -qx 'SELFTEST PASS' ||
    { echo "REFUSED: stepb_rtt_read.py --selftest does not pass" >&2; exit 1; }

if [ "$PC_PREFLIGHT" = 1 ]; then
    : "${PF_INSTALL:?PC_PREFLIGHT needs PF_INSTALL, the install directory of pc_preflight_build.sh}"
    [ -x "$PF_INSTALL/rmw_perf_pingpong/lib/rmw_perf_pingpong/large_rtt_tx" ] ||
        { echo "no large_rtt_tx under $PF_INSTALL" >&2; exit 64; }
    grep -q cpu_window_ms "$PF_INSTALL/rmw_perf_pingpong/lib/rmw_perf_pingpong/large_rtt_rx" ||
        { echo "REFUSED: $PF_INSTALL's large_rtt_rx has no CPU window (an install from before it)" >&2; exit 64; }
    CLIENT=sbrtt_$$; PEER_NS=sbrttp_$$; BCAST=192.168.77.255
    SHA=${SHA:-$(git -C "$REPO" rev-parse HEAD)}
    ROS_SETUP=/opt/ros/lyrical/setup.bash
    PF_LOCK=/tmp/stepb_rtt_pc_preflight.$USER.lock # the pid and log files under /tmp are this harness's: one at a time
    exec 8>>"$PF_LOCK" || { echo "REFUSED: cannot open $PF_LOCK" >&2; exit 75; }
    flock -n 8 || { echo "REFUSED: another PC preflight holds $PF_LOCK" >&2; exit 75; }
    HOME_BIN=$PF_INSTALL/rmw_perf_pingpong/lib/rmw_perf_pingpong
    TICKLE_LIB=$PF_INSTALL/rmw_tickle/lib/librmw_tickle.so
    BASE="set +u; source $ROS_SETUP; source $PF_INSTALL/local_setup.bash; set -u"
else
    SHA=${SHA:?set SHA to a commit on origin/main}
    git -C "$REPO" fetch -q origin main || { echo "REFUSED: cannot fetch origin main" >&2; exit 1; }
    SHA=$(git -C "$REPO" rev-parse --verify --quiet "$SHA^{commit}") || { echo "REFUSED: unknown SHA" >&2; exit 1; }
    git -C "$REPO" merge-base --is-ancestor "$SHA" origin/main || { echo "REFUSED: $SHA is not on origin/main" >&2; exit 1; }
    git -C "$REPO" grep -q 'RMW_TICKLE_LARGE_REGION' "$SHA" -- rmw_tickle/rmw_tickle/src/rmw_large.c ||
        { echo "REFUSED: $SHA has no RMW_TICKLE_LARGE_REGION (no step B to switch off)" >&2; exit 1; }
    git -C "$REPO" grep -q 'cpu_window_ms' "$SHA" -- rmw_tickle/rmw_perf_pingpong/src/large_rtt_rx.cpp ||
        { echo "REFUSED: $SHA's large_rtt_rx has no CPU window" >&2; exit 1; }
    export RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000}
    if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
        echo "$(date -Is) checks passed; waiting for the rig lock (hil, up to ${RIG_LOCK_WAIT} s)"
        exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
    fi
    # shellcheck source=examples/perf_hil/experiments/pi_checkout.sh
    # shellcheck disable=SC1091 # found from the script's own path at runtime
    . "$X/pi_checkout.sh"
    # shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
    # shellcheck disable=SC1091 # found from the script's own path at runtime
    . "$X/gate_v2_lib.sh"
    HOME_BIN=$PI_HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong
    TICKLE_LIB=$PI_HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so
    # Overlays with local_setup.bash only (a setup.bash re-sources its underlays and reorders the path).
    # shellcheck disable=SC2016 # expanded on the Pi
    BASE='set +u; source '$ROS_SETUP'; source $HOME/tickle/install/local_setup.bash; [ -f $HOME/rmw_perf_ws/install/local_setup.bash ] && source $HOME/rmw_perf_ws/install/local_setup.bash; set -u'
fi
OUT=${OUT:-$HOME/rig_results_safe/stepb_rtt_${SHA:0:8}_$(date +%Y%m%d-%H%M%S)}
R=$OUT.runs
mkdir -p "$R" || exit 1
SUM=$OUT.txt
say() { echo "$*" | tee -a "$SUM"; }

sh_() {
    if [ "$PC_PREFLIGHT" = 1 ]; then
        sudo -n ip netns exec "$CLIENT" sudo -n -u "$USER" bash -c "$*"
    else
        ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$CLIENT" "$@"
    fi
}
fetch_() { # remote local
    if [ "$PC_PREFLIGHT" = 1 ]; then cp "$1" "$2" 2>/dev/null; else scp -q -i "$K" -o BatchMode=yes "ci@$CLIENT:$1" "$2" 2>/dev/null; fi
}
qdisc_of() { # <dev>: its root qdisc
    if [ "$PC_PREFLIGHT" = 1 ]; then sudo -n ip netns exec "$CLIENT" tc qdisc show dev "$1"; else sh_ "tc qdisc show dev $1" </dev/null; fi
}

# stop_role <role> <binary name>: SIGINT the process this script launched as <role>, by the PID its launcher wrote,
# only if /proc/PID/exe is that binary.
stop_role() {
    # shellcheck disable=SC2016 # expanded on the host
    sh_ 'f=/tmp/sbrtt_'"$1"'.pid; [ -s $f ] || exit 0; p=$(cat $f)
        case "$(readlink /proc/$p/exe 2>/dev/null)" in */'"$2"') kill -INT "$p" ;; esac' </dev/null >/dev/null 2>&1
}
alive_role() { # <role> <binary>: 0 while that process runs
    # shellcheck disable=SC2016
    sh_ 'p=$(cat /tmp/sbrtt_'"$1"'.pid 2>/dev/null); [ -n "$p" ] && case "$(readlink /proc/$p/exe 2>/dev/null)" in */'"$2"') exit 0 ;; esac; exit 1' </dev/null
}

PC_VETH=sb$$
pc_netns_up() {
    sudo -n ip netns add "$CLIENT" && sudo -n ip netns add "$PEER_NS" || return 1
    sudo -n ip link add "${PC_VETH}c" type veth peer name "${PC_VETH}p" || return 1
    sudo -n ip link set "${PC_VETH}c" netns "$CLIENT" && sudo -n ip link set "${PC_VETH}p" netns "$PEER_NS" || return 1
    sudo -n ip -n "$CLIENT" link set "${PC_VETH}c" name eth0 &&
        sudo -n ip -n "$CLIENT" addr add 192.168.77.2/24 brd 192.168.77.255 dev eth0 &&
        sudo -n ip -n "$CLIENT" link set lo up && sudo -n ip -n "$CLIENT" link set eth0 up &&
        sudo -n ip -n "$PEER_NS" link set "${PC_VETH}p" up && sudo -n ip -n "$CLIENT" route add default dev eth0 || return 1
}

cleanup() { # the one EXIT trap
    stop_role tx large_rtt_tx
    stop_role rx large_rtt_rx
    if [ "$PC_PREFLIGHT" = 1 ]; then
        sleep 2
        sudo -n ip netns del "$CLIENT" 2>/dev/null
        sudo -n ip netns del "$PEER_NS" 2>/dev/null
    fi
    echo "STEPB_RTT_END $(date -Is) out=$OUT" >>"$SUM"
    echo "STEPB_RTT_END $(date -Is) out=$OUT"
}
trap cleanup EXIT
if [ "$PC_PREFLIGHT" = 1 ]; then pc_netns_up || { say "PC preflight: cannot build the namespaces"; exit 1; }; fi

say "=== step B same-host round trip, $(date -Is): SHA $SHA, $REPS reps, $SAMPLES samples a run, drain $DRAIN s," \
    "cells: $CELLS, arms: $ARMS, controls: $CONTROLS x $CONTROL_ARMS, pc preflight $PC_PREFLIGHT (host $CLIENT), out $OUT ==="
printf 'sha=%s\nreps=%s\nsamples=%s\ndrain=%s\ncells=%s\narms=%s\ncontrols=%s\ncontrol_arms=%s\npc_preflight=%s\n' \
    "$SHA" "$REPS" "$SAMPLES" "$DRAIN" "$CELLS" "$ARMS" "$CONTROLS" "$CONTROL_ARMS" "$PC_PREFLIGHT" >"$R/meta.txt"

# A shaped link at the start is someone's leftover: refuse rather than measure through it.
for dev in eth0 lo; do
    q=$(qdisc_of "$dev") || { say "REFUSED: cannot read the qdisc of $CLIENT $dev"; exit 1; }
    case "$q" in *netem* | *tbf*) say "REFUSED: $CLIENT $dev is shaped ($q)"; exit 1 ;; esac
    say "qdisc $CLIENT $dev: $(echo "$q" | head -1)"
done

if [ "$PC_PREFLIGHT" != 1 ]; then
    layout_cflags_default
    case "$TICKLE_EXTRA_CFLAGS" in *[!A-Za-z0-9=_\ -]*) say "TICKLE_EXTRA_CFLAGS may hold only [A-Za-z0-9=_ -]"; exit 64 ;; esac
    LAYOUT_BUILD="'-DCMAKE_C_FLAGS=$TICKLE_EXTRA_CFLAGS' '-DCMAKE_CXX_FLAGS=$TICKLE_EXTRA_CFLAGS'"
    RMW_TICKLE_BUILD="-DCMAKE_BUILD_TYPE=None '-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS' '-DCMAKE_CXX_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS'"
    say "--- building rmw_tickle + rmw_perf_pingpong at $SHA on $CLIENT (TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS') ---"
    # l4_rtt_rig.sh's three stages, in its order (builtin_interfaces/rcl_interfaces in rmw_perf_ws carry TickLE's
    # typesupport, so they are rebuilt against the typesupport just built, then rmw_perf_pingpong).
    if ! sh_ "set -e
cd ~/tickle && $(pi_checkout "$SHA") && git clean -fdqx -e install -e build -e log
export PYTHONPATH=\$HOME/tickle/tools/typesupport\${PYTHONPATH:+:\$PYTHONPATH}
set +u; source $ROS_SETUP; set -u
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
  --cmake-args -DBUILD_SHARED_LIBS=ON $RMW_TICKLE_BUILD > /tmp/sbrtt_build1.log 2>&1 \
  || { echo \"STAGE 1 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/sbrtt_build1.log; exit 1; }
set +u; source \$HOME/tickle/install/setup.bash; set -u
colcon build --base-paths \$HOME/rmw_perf_ws --build-base \$HOME/rmw_perf_ws/build --install-base \$HOME/rmw_perf_ws/install \
  --packages-select builtin_interfaces rcl_interfaces --cmake-args -DCMAKE_BUILD_TYPE=Release $LAYOUT_BUILD --cmake-force-configure > /tmp/sbrtt_build2.log 2>&1 \
  || { echo \"STAGE 2 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/sbrtt_build2.log; exit 1; }
set +u; source \$HOME/rmw_perf_ws/install/setup.bash; source \$HOME/tickle/install/setup.bash; set -u
colcon build --packages-select rmw_perf_pingpong --cmake-args -DCMAKE_BUILD_TYPE=Release $LAYOUT_BUILD > /tmp/sbrtt_build3.log 2>&1 \
  || { echo \"STAGE 3 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/sbrtt_build3.log; exit 1; }
test -f $TICKLE_LIB && test -x $HOME_BIN/large_rtt_tx && test -x $HOME_BIN/large_rtt_rx
echo \"built on \$(hostname) at \$(git rev-parse --short HEAD)\"" </dev/null 2>&1 | tee -a "$SUM"; then
        say "BUILD FAILED on $CLIENT - not running"; exit 1
    fi
    at=$(sh_ "git -C \$HOME/tickle rev-parse HEAD; grep -c cpu_window_ms $HOME_BIN/large_rtt_rx" </dev/null)
    case "$at" in "$SHA"*[1-9]*) ;; *) say "BUILD FAILED - $CLIENT is not at $SHA with the CPU-window binaries"; exit 1 ;; esac
    say "librmw_tickle.so on $CLIENT: $(sh_ "sha256sum $TICKLE_LIB | cut -c1-16" </dev/null)"
fi

env_for() {
    echo "$BASE"
    echo "unset RMW_TICKLE_LARGE_REGION RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS RMW_TICKLE_PUBLISH_HUGEPAGE RMW_TICKLE_MAX_SAMPLE_BYTES"
    echo "unset TT_UDP_OFFLOAD TT_UDP_CSUM0 TT_RX_DIRECT RMW_TICKLE_LOANS PINGPONG_LOANED_PUBLISH"
    echo "unset FASTRTPS_DEFAULT_PROFILES_FILE FASTDDS_DEFAULT_PROFILES_FILE RMW_FASTRTPS_USE_QOS_FROM_XML RMW_FASTRTPS_PUBLICATION_MODE CYCLONEDDS_URI"
    case "$1" in
        tickle@A | tickle@A2) echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=$BCAST RMW_TICKLE_LARGE_REGION=0" ;;
        tickle@B) echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=$BCAST" ;;
        fastdds) echo "export RMW_IMPLEMENTATION=rmw_fastrtps_cpp" ;;
        cyclonedds) echo "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp" ;;
    esac
    echo "export ROS_DOMAIN_ID=$DOMAIN"
}
rmw_of() { case "$1" in tickle@*) echo rmw_tickle ;; fastdds) echo rmw_fastrtps_cpp ;; cyclonedds) echo rmw_cyclonedds_cpp ;; esac; }

# The launcher: the process detached, its PID written by itself just before exec (never found by a pattern).
# shellcheck disable=SC2016 # expanded on the host
LAUNCHER='#!/bin/bash
role=$1; envfile=$2; shift 2
source "$envfile"
rm -f /tmp/sbrtt_$role.log /tmp/sbrtt_$role.pid /tmp/sbrtt_$role.maps /tmp/sbrtt_$role.large /tmp/sbrtt_$role.samples
setsid nohup bash -c "echo \$\$ > /tmp/sbrtt_$role.pid; exec \"\$0\" \"\$@\"" "$@" > /tmp/sbrtt_$role.log 2>&1 < /dev/null &
for i in $(seq 1 50); do [ -s /tmp/sbrtt_$role.pid ] && break; sleep 0.1; done
cat /tmp/sbrtt_$role.pid'
printf '%s\n' "$LAUNCHER" | sh_ "cat > /tmp/sbrtt_launch.sh && chmod +x /tmp/sbrtt_launch.sh" || { say "launcher copy failed"; exit 1; }
for a in $ARMS $CONTROL_ARMS; do
    env_for "$a" | sh_ "cat > /tmp/sbrtt_env_${a/@/_}.sh" || { say "env copy failed"; exit 1; }
done

# maps_ok <maps file> <arm>: the arm's rmw library is loaded, and for rmw_tickle exactly the one built here and no other.
maps_ok() {
    case "$2" in
        tickle@*) grep -qx "$TICKLE_LIB" "$1" && ! grep 'librmw_tickle\.so' "$1" | grep -vqx "$TICKLE_LIB" ;;
        fastdds) grep -q '/librmw_fastrtps_cpp\.so' "$1" ;;
        cyclonedds) grep -q '/librmw_cyclonedds_cpp\.so' "$1" ;;
    esac
}

# read_maps <role>: the rmw libraries in that process's /proc/PID/maps, polled until an rmw implementation is loaded
# (at rclcpp::init) or 15 s pass, into /tmp/sbrtt_<role>.maps.
read_maps() {
    # shellcheck disable=SC2016 # expanded on the host
    sh_ 'p=$(cat /tmp/sbrtt_'"$1"'.pid); for i in $(seq 1 75); do
        grep -o "/[^ ]*librmw_[a-z_]*\.so" /proc/$p/maps 2>/dev/null | sort -u > /tmp/sbrtt_'"$1"'.maps
        grep -qE "/librmw_(tickle|fastrtps_cpp|cyclonedds_cpp)\.so$" /tmp/sbrtt_'"$1"'.maps && break; sleep 0.2; done' </dev/null
}

# read_large: appends the /dev/shm/tickle-large-* regions each process maps now to /tmp/sbrtt_<role>.large, and a line
# "read <role> ok|gone" saying whether the process could be looked at (a gone process is "could not look", not "none").
read_large() {
    # shellcheck disable=SC2016 # expanded on the host
    sh_ 'for role in tx rx; do p=$(cat /tmp/sbrtt_$role.pid 2>/dev/null)
        if [ -n "$p" ] && [ -r /proc/$p/maps ]; then grep -o "tickle-large-[^ ]*" /proc/$p/maps 2>/dev/null | sort -u >> /tmp/sbrtt_$role.large
            echo "read $role ok" >> /tmp/sbrtt_$role.large; else echo "read $role gone" >> /tmp/sbrtt_$role.large; fi; done' </dev/null
}

run_one() { # rep slot cell arm
    local rep=$1 slot=$2 cell=$3 arm=$4 msg qos depth rate bytes dur stem status="ok" t0 deadline envf want txr rxr txl rxl
    read -r msg qos depth rate bytes <<<"$(cell_def "$cell")"
    dur=$((SAMPLES / rate))
    stem="r${rep}s${slot}_${cell}_${arm/@/_}"
    envf=/tmp/sbrtt_env_${arm/@/_}.sh
    sh_ "/tmp/sbrtt_launch.sh rx $envf $HOME_BIN/large_rtt_rx -m $msg --qos $qos --depth $depth" </dev/null >/dev/null
    read_maps rx
    sleep 1
    sh_ "/tmp/sbrtt_launch.sh tx $envf $HOME_BIN/large_rtt_tx -m $msg --rate $rate -d $dur --qos $qos --depth $depth --drain $DRAIN --samples /tmp/sbrtt_tx.samples" </dev/null >/dev/null
    t0=$(date +%s)
    read_maps tx
    # The treatment, seen twice inside the schedule (match + warm-up take ~2-4 s; the schedule is 30 s), every arm alike.
    for at in 8 16; do
        while [ "$(date +%s)" -lt $((t0 + at)) ]; do sleep 0.2; done
        read_large
    done
    deadline=$((t0 + dur + 30 + 20 + DRAIN + 30))
    while alive_role tx large_rtt_tx; do
        if [ "$(date +%s)" -ge "$deadline" ]; then status="VOID:timeout"; stop_role tx large_rtt_tx; sleep 2; break; fi
        sleep 1
    done
    stop_role rx large_rtt_rx
    for _ in 1 2 3 4 5 6 7 8 9 10; do alive_role rx large_rtt_rx || break; sleep 0.5; done
    for f in tx.log tx.samples tx.maps tx.large rx.log rx.maps rx.large; do fetch_ "/tmp/sbrtt_$f" "$R/${stem}_$f"; done
    want=$(rmw_of "$arm")
    if [ "$status" = ok ] && ! grep -q "^RESULT: " "$R/${stem}_tx.log" 2>/dev/null &&
        grep -q "^no warm-up round trip" "$R/${stem}_tx.log" 2>/dev/null; then
        # Matched, and not one sample answered in 20 s: the arm does not deliver in this cell (step A's same-host
        # BEST_EFFORT 4 MB on the PC dry run). Not VOID - not delivering is the finding - if its identity holds.
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
    # Treatment facts for the reader: regions mapped (1/0) per process, and whether both reads could look (ok/gone).
    txr=$(grep -c '^tickle-large-' "$R/${stem}_tx.large" 2>/dev/null); rxr=$(grep -c '^tickle-large-' "$R/${stem}_rx.large" 2>/dev/null)
    txl=$(grep -c '^read tx ok' "$R/${stem}_tx.large" 2>/dev/null); rxl=$(grep -c '^read rx ok' "$R/${stem}_rx.large" 2>/dev/null)
    echo "RUN rep=$rep slot=$slot cell=$cell arm=$arm stem=$stem status=$status msg=$msg qos=$qos depth=$depth rate=$rate" \
        "bytes=$bytes dur=$dur tx_region=$(( ${txr:-0} > 0 )) rx_region=$(( ${rxr:-0} > 0 )) tx_looked=${txl:-0}" \
        "rx_looked=${rxl:-0}" >>"$R/index.txt"
    say "RUN $stem $status region tx=${txr:-0} rx=${rxr:-0} | $(grep -h '^RESULT:' "$R/${stem}_tx.log" 2>/dev/null |
        grep -oE ' (sent=[0-9]+ replied=[0-9]+|rtt_p50_ms=[0-9.-]+|cpu_window_ms=[0-9.]+)' | tr '\n' ' ')|$(grep -h '^RX:' \
        "$R/${stem}_rx.log" 2>/dev/null | grep -oE ' (cpu_window_ms=[0-9.]+|cpu_window_takes=[0-9]+)' | tr '\n' ' ')"
}

# run_cells <rep> <slot> <cells> <arms>: every cell, every arm, the arm order rotated by the rep.
run_cells() {
    local rep=$1 slot=$2 cell i arms_a na
    read -r -a arms_a <<<"$4"; na=${#arms_a[@]}
    for cell in $3; do
        for i in $(seq 0 $((na - 1))); do
            run_one "$rep" "$slot" "$cell" "${arms_a[$(((i + rep - 1) % na))]}"
        done
    done
}
for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep/$REPS $(date -Is) ---"
    run_cells "$rep" 1 "$CONTROLS" "$CONTROL_ARMS"
    run_cells "$rep" 1 "$CELLS" "$ARMS"
    run_cells "$rep" 2 "$CONTROLS" "$CONTROL_ARMS"
done
python3 "$X/stepb_rtt_read.py" "$OUT" >"$OUT.verdicts.txt" 2>&1
rc=$?
tee -a "$SUM" <"$OUT.verdicts.txt"
say "STEPB_RTT_DONE reader_rc=$rc $(date -Is)"
