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
#   LOSS (changed 2026-10-06, before the next run): perf_test counts every id below the first one it receives as
#     lost, and the rig's build compiles --expected_num_subs out, so samples published before the match showed as
#     loss for every arm. Loss is now counted from the second after the first delivery; the first delivered
#     second's gap is reported apart as the pre-match gap, and the old all-rows figure is kept beside it.
#
# Also driven by fastdds_keepall_arms.sh (Fast DDS QoS arms): ARMS may name fastdds@<F> (F0 = fastdds_eth0_only.xml,
# F<n> = fastdds/fastdds_keepall_F<n>.xml, copied from THIS checkout to /tmp/ka_fdds_<F>.xml on both Pis), CELLS
# may list topic:loss pairs instead of TOPICS x LOSSES, and SUMMARY names the summary script. With no tickle arm in
# ARMS nothing is built and HEAD_SHA is not needed.
#
# EQUAL_BOUND (docs/ROADMAP.md "Now" 0, open item (a); the user's rule of 2026-10-06): KEEP_ALL at equal conditions,
# DDS bounded in samples and rmw_tickle in bytes worth the same samples, as campaign_sweep.sh's common_args does for
# the native benches. Unset, nothing below changes. EQUAL_BOUND=auto picks N per topic by campaign_sweep.sh's rule,
# N = min(2048, floor(512 KiB / one sample's arena footprint)) - what rmw_tickle's shipped 512 KiB budget holds, so
# rmw_tickle barely moves and the vendors are brought to the same count; EQUAL_BOUND=<n> uses n for every topic.
# At the rig's build: Array1k N = 492, Array4k N = 125 (EXPECTED_N below; a different computed N stops the run, since
# the documents quote these). Default ARMS become "tickle@head fastdds cyclonedds"; tickle@pre/ack and fastdds@<F>
# are refused (the variants' sizing code predates this arithmetic, and a QoS arm is a second treatment).
#   rmw_tickle  RMW_TICKLE_KEEP_ALL_BYTES = (N + 1) x footprint, footprint = tt_sample_cache_bytes(encoded + the 4-byte
#               psn every sample of a run carries), computed on the client Pi by rmw_keepall_bound.c against the core
#               headers just built. The arena keeps one sample of wrap slack (keep_all_arena_bytes() in the native
#               bench, resolve_keep_all_arena_bytes() in rmw_publisher.c), so that budget holds exactly N; the count
#               bounds (the subscription's 1,024-datagram window, the 2,048-slot ring, both / datagrams per sample)
#               must not bind first, or the run is refused before it starts.
#   fastdds     fastdds/fastdds_keepall_equal.xml.template with @N@ = N: a default data_writer profile with
#               resource_limits max_samples = max_samples_per_instance = N, max_instances 1, validated with xmllint
#               against the client Pi's /opt/ros/jazzy/share/fastRTPS_profiles.xsd before anything runs.
#   cyclonedds  NOT bounded, labelled so: rmw_cyclonedds_cpp 2.2.3 (rmw_node.cpp create_readwrite_qos) sets
#               KEEP_ALL and never calls dds_qset_resource_limits, so the writer keeps CycloneDDS's unlimited
#               default, and CycloneDDS 0.10.5's configuration (CYCLONEDDS_URI) carries no QoS (no QoS provider in
#               0.10)
#               - its only writer-history bounds are Internal/Watermarks/WhcHigh/WhcLow, in bytes of unacknowledged
#               data (default 500 kB), not samples. Bounding it in samples would need code.
# HOW TO READ IT (enforced by the summary, which reads $OUT.runs/bounds.txt): a run is VOID unless its publisher's
# /proc/PID/environ at 2 s shows the arm's treatment - rmw_tickle: RMW_TICKLE_KEEP_ALL_BYTES equal to the recorded
# budget, and min(budget / footprint - 1, window, ring) == N (derived: rmw_tickle logs no realized arena); Fast DDS:
# FASTRTPS_DEFAULT_PROFILES_FILE naming the N profile with its recorded sha256, no RMW_FASTRTPS_USE_QOS_FROM_XML, and
# no Fast DDS XML or QoS-check error in the log (a profile it rejects leaves the 5,000 default). CycloneDDS rows are
# printed as "default, not equal". Before the real runs, under the same lock, a dry run (DRY=1, the default in this
# mode) runs every arm once per topic for DRY_DUR s at 0% loss; any VOID there stops the job.
# USAGE (detached, from this checkout; HEAD_SHA must be pushed, the profile template and helper are copied from here):
#   TS=$(date +%Y%m%d-%H%M%S) OUT=$HOME/rig_results_safe/rmw_keepall_equal_$TS; setsid nohup env EQUAL_BOUND=auto \
#     HEAD_SHA=<sha> OUT="$OUT" examples/perf_hil/experiments/rmw_keepall_rig.sh > "$OUT.launch.log" 2>&1 < /dev/null &
# DURATION: build 2-5 min (head only; 2 min incremental on 2026-10-06), dry run 6 runs x ~20 s = 2 min, then 3 arms x
#   4 cells x 3 reps = 36 runs at ~32 s (measured 2026-10-06) = ~19 min: estimate 30 min, analyse an overrun at 45.
#
# Added 2026-10-10 for ~/rig_queue_largemsg_A4.sh (large-message step A, L3 and L4; docs/DESIGN.md section 8). Each is
# off unless set, so every earlier invocation runs unchanged:
#   fastdds@mms1472  Fast DDS with fastdds/fastdds_eth0_only_mms1472.xml (eth0 only, maxMessageSize 1472: the design's
#                    vendor-favouring second arm). A3's queue named fastdds_keepall_F1472.xml, which never existed, and
#                    skipped the arm without failing.
#   CLOCK_PROBE=1    clock_offset_probe.py between the two Pis over eth0, before and after every run, into
#                    <stem>_clock.txt; the launcher also records each perf_test's start (<stem>_<role>.start, wall ns).
#                    perf_test's cross-host latency is subscriber clock minus publisher clock; the summary
#                    (largemsg_rig_summary.py) subtracts the measured offset, interpolated in time, to put it on one clock.
#   SAMEHOST=1       publisher and subscriber both on the client Pi. Every vendor at its shipped same-host default (DDS
#                    default first, docs/TESTING.md section 5): no Fast DDS profile (its SHM transport on), no
#                    CYCLONEDDS_URI; rmw_tickle with TICKLE_BROADCAST_ADDR, as rmw_samehost.sh's scored arm. fastdds@<F>
#                    arms, EQUAL_BOUND and lossy cells are refused (a profile is a cross-host treatment; tc acts on
#                    eth0, which a same-host sample never crosses). One clock: no probe.
#   PC_PREFLIGHT=1   no rig and no lock: "client" and "server" are two private network namespaces joined by a veth named
#                    eth0 in each (192.168.10.2 and .3, broadcast .255, a default route), every Pi command runs in its
#                    namespace as this user, files are copied instead of scp'd, nothing is built, and the stack is the
#                    PC's: PF_TICKLE_INSTALL / PF_PERF_WS (pc_preflight_build.sh's install and perf/install) and
#                    PF_VARIANT_<name> (an install dir holding rmw_tickle for arm tickle@<name>). It tests this script,
#                    the launcher, the probe and the summary on the same code path as the rig, not the rig's build.
#   meta.txt         the run's QoS flags, rate, SAMEHOST, CLOCK_PROBE, DUR and DRY, for the summary.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
PC_PREFLIGHT=${PC_PREFLIGHT:-0}
if [ "$PC_PREFLIGHT" != 1 ] && [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi

K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
SAMEHOST=${SAMEHOST:-0}; CLOCK_PROBE=${CLOCK_PROBE:-0}; PROBE_PORT=${PROBE_PORT:-39117}
if [ "$PC_PREFLIGHT" = 1 ]; then
    : "${PF_TICKLE_INSTALL:?PC_PREFLIGHT needs PF_TICKLE_INSTALL (pc_preflight_build.sh)}"
    : "${PF_PERF_WS:?PC_PREFLIGHT needs PF_PERF_WS (pc_preflight_build.sh)}"
    CLIENT=ka_pfc_$$; SERVER=ka_pfs_$$; SKIP_BUILD=1
fi
[ "$SAMEHOST" = 1 ] && SERVER=$CLIENT
# Each distinct host once: under SAMEHOST the client is the server, and two builds or copies at once on one Pi would
# write the same tree.
HOSTS=("$CLIENT")
[ "$SERVER" = "$CLIENT" ] || HOSTS+=("$SERVER")
EQUAL_BOUND=${EQUAL_BOUND:-}
if [ -n "$EQUAL_BOUND" ]; then
    case "$EQUAL_BOUND" in auto | [1-9] | [1-9][0-9] | [1-9][0-9][0-9] | [1-9][0-9][0-9][0-9]) ;;
        *) echo "EQUAL_BOUND must be auto or a sample count 1..9999, not '$EQUAL_BOUND'" >&2; exit 64 ;; esac
    ARMS=${ARMS:-"tickle@head fastdds cyclonedds"}
    for a in $ARMS; do
        case "$a" in tickle@head | fastdds | cyclonedds) ;;
            *) echo "EQUAL_BOUND runs tickle@head, fastdds and cyclonedds only, not $a" >&2; exit 64 ;; esac
    done
fi
# The N campaign_sweep.sh's rule gives at the rig's build, quoted in the documents: a different computed N stops the
# run (the typesupport or core's sizing changed, and the quoted figure would be stale).
EXPECTED_N="Array1k:492 Array4k:125"
ARMS=${ARMS:-"tickle@pre tickle@ack tickle@head fastdds cyclonedds"}
if [ "$SAMEHOST" = 1 ]; then
    [ -z "$EQUAL_BOUND" ] || { echo "SAMEHOST does not run EQUAL_BOUND" >&2; exit 64; }
    for a in $ARMS; do
        case "$a" in fastdds@*) echo "SAMEHOST runs every vendor at its shipped default, not $a" >&2; exit 64 ;; esac
    done
fi
HAS_TICKLE=0; case " $ARMS " in *" tickle@"*) HAS_TICKLE=1 ;; esac
if [ "$HAS_TICKLE" = 1 ]; then HEAD_SHA=${HEAD_SHA:?set HEAD_SHA to a pushed commit}; else HEAD_SHA=${HEAD_SHA:-none}; fi
PRE_SHA=${PRE_SHA:-674f0dcb}; ACK_SHA=${ACK_SHA:-8d1c3712}
REPS=${REPS:-3}; DUR=${DUR:-20}; TOPICS=${TOPICS:-"Array1k Array4k"}; LOSSES=${LOSSES:-"0 5"}
if [ -z "${CELLS:-}" ]; then
    CELLS=""; for l in $LOSSES; do for t in $TOPICS; do CELLS="$CELLS $t:$l"; done; done
fi
if [ "$SAMEHOST" = 1 ]; then
    for c in $CELLS; do [ "${c#*:}" = 0 ] || { echo "SAMEHOST cells are lossless, not $c" >&2; exit 64; }; done
fi
SUMMARY=${SUMMARY:-$REPO/examples/perf_hil/experiments/rmw_keepall_rig_summary.py}
DOMAIN=${DOMAIN:-61}
# QOS_ARGS and PUB_RATE (2026-10-09, large-message stage 2's L4 cells, docs/DESIGN.md section 8): perf_test's QoS flags
# and the publisher's rate in Hz. The defaults are this script's own cell exactly - RELIABLE KEEP_ALL at max rate -
# so every earlier invocation runs unchanged. ~/rig_queue_largemsg_A.sh sets "--reliable --keep_last --history_depth
# 10" (ROS's default) or "--keep_last --history_depth 5" (sensor_data) and 30 or 15.
QOS_ARGS=${QOS_ARGS:---reliable}
PUB_RATE=${PUB_RATE:-0}
OUT=${OUT:-$HOME/rig_results_safe/rmw_keepall_rig_$(date +%Y%m%d-%H%M%S)}
if [ -n "$EQUAL_BOUND" ]; then DRY=${DRY:-1}; else DRY=${DRY:-0}; fi
DRY_DUR=${DRY_DUR:-6}
mkdir -p "$OUT.runs"
[ "$DRY" = 1 ] && mkdir -p "$OUT.dry"
SUM="$OUT.txt"
sh_() {
    local h=$1; shift
    if [ "$PC_PREFLIGHT" = 1 ]; then # the "host" is a namespace on this PC; the command runs there as this user
        sudo -n ip netns exec "$h" sudo -n -u "$USER" bash -c "$*"
    else
        ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "$@"
    fi
}
fetch_() { # host remote-path local-path
    if [ "$PC_PREFLIGHT" = 1 ]; then cp "$2" "$3" 2>/dev/null; else scp -q -i "$K" -o BatchMode=yes "ci@$1:$2" "$3" 2>/dev/null; fi
}
say() { echo "$*" | tee -a "$SUM"; }
PC_VETH=ka$$
pc_netns_up() {
    local ns ip peer
    sudo -n ip netns add "$CLIENT" && sudo -n ip netns add "ka_pfs_$$" || return 1
    sudo -n ip link add "${PC_VETH}c" type veth peer name "${PC_VETH}s" || return 1
    for ns in "$CLIENT" "ka_pfs_$$"; do
        case "$ns" in "$CLIENT") peer=${PC_VETH}c ip=192.168.10.2 ;; *) peer=${PC_VETH}s ip=192.168.10.3 ;; esac
        sudo -n ip link set "$peer" netns "$ns" && sudo -n ip -n "$ns" link set "$peer" name eth0 &&
            sudo -n ip -n "$ns" addr add "$ip/24" brd 192.168.10.255 dev eth0 && sudo -n ip -n "$ns" link set lo up &&
            sudo -n ip -n "$ns" link set eth0 up && sudo -n ip -n "$ns" route add default dev eth0 || return 1
    done
}
pc_netns_down() {
    sudo -n ip netns del "$CLIENT" 2>/dev/null
    sudo -n ip netns del "ka_pfs_$$" 2>/dev/null
    return 0
}

set_loss() {
    if [ "$PC_PREFLIGHT" = 1 ]; then # tc as root through ip netns exec (sudo -n permits ip; a nested sudo tc does not)
        if [ "$1" = 0 ]; then sudo -n ip netns exec "$CLIENT" tc qdisc del dev eth0 root >/dev/null 2>&1 || true
        else sudo -n ip netns exec "$CLIENT" tc qdisc replace dev eth0 root netem loss "$1%"; fi
        return
    fi
    if [ "$1" = 0 ]; then sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" </dev/null >/dev/null 2>&1 || true
    else sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss $1%" </dev/null; fi
}
cleanup() {
    set_loss 0
    for h in "${HOSTS[@]}"; do
        # shellcheck disable=SC2016 # expanded on the Pi, not here
        sh_ "$h" 'for f in /tmp/ka_*.pid; do [ -f "$f" ] || continue; p=$(cat "$f");
            case "$(readlink /proc/$p/exe 2>/dev/null)" in */perf_test) kill -INT "$p";; esac; rm -f "$f"; done' \
            </dev/null >/dev/null 2>&1
    done
    if [ "$PC_PREFLIGHT" = 1 ]; then sleep 2; pc_netns_down; fi
}
trap cleanup EXIT
if [ "$PC_PREFLIGHT" = 1 ]; then pc_netns_up || { echo "PC preflight: cannot build the namespaces"; exit 1; }; fi

say "=== rmw KEEP_ALL on the rig, $(date -Is): head $HEAD_SHA, ack $ACK_SHA, pre $PRE_SHA, $REPS reps, ${DUR}s," \
    "cells:$CELLS, arms: $ARMS, out $OUT ==="
[ -n "$EQUAL_BOUND" ] && say "EQUAL_BOUND=$EQUAL_BOUND: every KEEP_ALL writer bounded alike (see the header); dry run $DRY"
say "qos [$QOS_ARGS] rate $PUB_RATE Hz; samehost $SAMEHOST; clock probe $CLOCK_PROBE; pc preflight $PC_PREFLIGHT" \
    "(client $CLIENT, server $SERVER)"
printf 'qos_args=%s\npub_rate=%s\nsamehost=%s\nclock_probe=%s\ndur=%s\ndry=%s\npc_preflight=%s\n' "$QOS_ARGS" \
    "$PUB_RATE" "$SAMEHOST" "$CLOCK_PROBE" "$DUR" "$DRY" "$PC_PREFLIGHT" > "$OUT.runs/meta.txt"
[ "$HAS_TICKLE" = 1 ] || SKIP_BUILD=1
# The pre/ack variants to build: only those ARMS names (the default ARMS names both, as before).
# Where each tickle build is installed: on the Pis ~/tickle/install and ~/rmw_variants/<name>/install; in the PC
# preflight the PC's builds (header, PC_PREFLIGHT).
head_install() { if [ "$PC_PREFLIGHT" = 1 ]; then echo "$PF_TICKLE_INSTALL"; else echo /home/ci/tickle/install; fi; }
variant_install() { # $1 variant name
    if [ "$PC_PREFLIGHT" = 1 ]; then local v="PF_VARIANT_$1"; echo "${!v:?PC_PREFLIGHT needs $v for arm tickle@$1}"
    else echo "/home/ci/rmw_variants/$1/install"; fi
}
HEAD_LIB="$(head_install)/rmw_tickle/lib/librmw_tickle.so"
VARIANT_SPECS=""; VARIANT_LIBS=""; N_TICKLE_LIBS=1
for v in pre:$PRE_SHA ack:$ACK_SHA; do
    case " $ARMS " in *" tickle@${v%%:*} "*)
        VARIANT_SPECS="$VARIANT_SPECS $v"
        VARIANT_LIBS="$VARIANT_LIBS $(variant_install "${v%%:*}")/rmw_tickle/lib/librmw_tickle.so"
        N_TICKLE_LIBS=$((N_TICKLE_LIBS + 1)) ;;
    esac
done

# ---- build: head into ~/tickle/install (typesupport too), pre and ack as librmw_tickle.so-only variants ----------
# rmw_tickle is built the way the vendor rmws it is compared with were (fairness audit, 2026-10-05): the jazzy debs
# export their CMake targets as "-none" - CMAKE_BUILD_TYPE None, Debian's -O2, no NDEBUG - while this built rmw_tickle
# as Release, -O3 -DNDEBUG. -g -O2 is the level they share. perf_test is one binary for every arm and stays Release.
RMW_TICKLE_BUILD="-DCMAKE_BUILD_TYPE=None '-DCMAKE_C_FLAGS=-g -O2' '-DCMAKE_CXX_FLAGS=-g -O2'"
if [ "${SKIP_BUILD:-0}" != 1 ]; then
say "--- building on both Pis ---"
pids=()
for h in "${HOSTS[@]}"; do
    sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $HEAD_SHA && git clean -fdqx -e install -e build -e log
export PYTHONPATH=\$HOME/tickle/tools/typesupport\${PYTHONPATH:+:\$PYTHONPATH}
set +u; source /opt/ros/jazzy/setup.bash; set -u
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
  --cmake-args -DBUILD_SHARED_LIBS=ON $RMW_TICKLE_BUILD > /tmp/ka_build1.log 2>&1 \
  || { echo \"STAGE 1 FAILED on \$(hostname)\"; tail -25 /tmp/ka_build1.log; exit 1; }
set +u; source \$HOME/tickle/install/setup.bash; set -u
# The interface packages' TickLE bindings are regenerated with this generator: perf_test's were from 2026-09-20.
colcon build --base-paths \$HOME/rmw_perf_ws --build-base \$HOME/rmw_perf_ws/build --install-base \$HOME/rmw_perf_ws/install \
  --packages-select builtin_interfaces rcl_interfaces performance_test --cmake-args -DCMAKE_BUILD_TYPE=Release \
  --cmake-force-configure > /tmp/ka_build2.log 2>&1 \
  || { echo \"STAGE 2 FAILED on \$(hostname)\"; tail -25 /tmp/ka_build2.log; exit 1; }
rm -rf \$HOME/rmw_variants; git worktree prune
for v in $VARIANT_SPECS; do
  name=\${v%%:*}; sha=\${v#*:}
  git worktree add -q --detach \$HOME/rmw_variants/\$name/src \$sha
  colcon build --base-paths \$HOME/rmw_variants/\$name/src --build-base \$HOME/rmw_variants/\$name/build \
    --install-base \$HOME/rmw_variants/\$name/install --packages-select rmw_tickle \
    --cmake-args -DBUILD_SHARED_LIBS=ON $RMW_TICKLE_BUILD > /tmp/ka_build_\$name.log 2>&1 \
    || { echo \"VARIANT \$name FAILED on \$(hostname)\"; tail -25 /tmp/ka_build_\$name.log; exit 1; }
done
echo \"built on \$(hostname): head \$(git rev-parse --short HEAD)\"
for v in $VARIANT_SPECS; do echo \"  variant \${v%%:*} \$(git -C \$HOME/rmw_variants/\${v%%:*}/src rev-parse --short HEAD)\"; done
sha256sum \$HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so $VARIANT_LIBS" \
        </dev/null 2>&1 | tee -a "$SUM" &
    pids+=($!)
done
bad=0; for p in "${pids[@]}"; do wait "$p" || bad=1; done
[ "$bad" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }
fi

# The tickle binaries (head and each variant ARMS names; all three by default) must differ, on each Pi, or the arms
# are one binary under several names.
[ "$HAS_TICKLE" = 1 ] && for h in "${HOSTS[@]}"; do
    n=$(sh_ "$h" "sha256sum $HEAD_LIB $VARIANT_LIBS | awk '{print \$1}' | sort -u | wc -l" </dev/null)
    if [ "$n" != "$N_TICKLE_LIBS" ]; then say "VOID OVERALL: $h has $n distinct librmw_tickle.so, not $N_TICKLE_LIBS"; exit 1; fi
done
[ "$HAS_TICKLE" = 1 ] && say "identity: $N_TICKLE_LIBS distinct librmw_tickle.so on both Pis"

CDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General></Domain></CycloneDDS>'
FDDS_PROFILE=/home/ci/tickle/examples/perf_hil/fastdds/fastdds_eth0_only.xml
[ "$PC_PREFLIGHT" = 1 ] && FDDS_PROFILE=$REPO/examples/perf_hil/fastdds/fastdds_eth0_only.xml
# Overlays are sourced with local_setup.bash only: a setup.bash re-sources its build-time underlays, which moves
# /opt/ros/jazzy back in front of rmw_perf_ws and loads interface packages without TickLE typesupport.
# shellcheck disable=SC2016 # expanded on the Pi, not here
BASE='set +u; source /opt/ros/jazzy/setup.bash; source $HOME/tickle/install/local_setup.bash
source $HOME/rmw_perf_ws/install/local_setup.bash'
if [ "$PC_PREFLIGHT" = 1 ]; then
    BASE="set +u; source /opt/ros/lyrical/setup.bash; source $PF_TICKLE_INSTALL/local_setup.bash
source $PF_PERF_WS/local_setup.bash; export KA_PERF_TEST=$PF_PERF_WS/performance_test/lib/performance_test/perf_test"
fi
env_for() {
    echo "$BASE"
    if [ "$SAMEHOST" = 1 ]; then # every vendor at its shipped same-host default (header, SAMEHOST)
        echo "unset FASTRTPS_DEFAULT_PROFILES_FILE FASTDDS_DEFAULT_PROFILES_FILE RMW_FASTRTPS_USE_QOS_FROM_XML"
        echo "unset RMW_FASTRTPS_PUBLICATION_MODE CYCLONEDDS_URI"
        case "$1" in
            tickle@head) echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
            tickle@*) echo "source $(variant_install "${1#tickle@}")/local_setup.bash"
                echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
            fastdds) echo "export RMW_IMPLEMENTATION=rmw_fastrtps_cpp" ;;
            cyclonedds) echo "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp" ;;
        esac
        echo "export ROS_DOMAIN_ID=$DOMAIN"
        return 0
    fi
    if [ -n "$EQUAL_BOUND" ]; then # $2 = topic: the bound is per topic (header, EQUAL_BOUND)
        case "$1" in
            tickle@head) echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255"
                echo "export RMW_TICKLE_KEEP_ALL_BYTES=${EQ_BYTES[$2]}" ;;
            fastdds) echo "unset RMW_FASTRTPS_USE_QOS_FROM_XML RMW_FASTRTPS_PUBLICATION_MODE FASTDDS_DEFAULT_PROFILES_FILE"
                echo "export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTRTPS_DEFAULT_PROFILES_FILE=/tmp/ka_fdds_eq${EQ_N[$2]}.xml" ;;
            cyclonedds) echo "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp CYCLONEDDS_URI='$CDDS_URI'" ;;
        esac
        echo "export ROS_DOMAIN_ID=$DOMAIN"
        return 0
    fi
    case "$1" in
        tickle@head) echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
        tickle@*) echo "source $(variant_install "${1#tickle@}")/local_setup.bash"
            echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
        fastdds) echo "export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTRTPS_DEFAULT_PROFILES_FILE=$FDDS_PROFILE" ;;
        # The QoS arms: the profile is the only treatment, so nothing else that steers rmw_fastrtps may leak in.
        fastdds@*) echo "unset RMW_FASTRTPS_USE_QOS_FROM_XML RMW_FASTRTPS_PUBLICATION_MODE FASTDDS_DEFAULT_PROFILES_FILE"
            echo "export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTRTPS_DEFAULT_PROFILES_FILE=/tmp/ka_fdds_${1#fastdds@}.xml" ;;
        cyclonedds) echo "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp CYCLONEDDS_URI='$CDDS_URI'" ;;
    esac
    echo "export ROS_DOMAIN_ID=$DOMAIN"
}
lib_for() {
    case "$1" in
        tickle@head) echo "$HEAD_LIB" ;;
        tickle@*) echo "$(variant_install "${1#tickle@}")/rmw_tickle/lib/librmw_tickle.so" ;;
        fastdds | fastdds@*) echo "librmw_fastrtps_cpp.so" ;;
        cyclonedds) echo "librmw_cyclonedds_cpp.so" ;;
    esac
}
fdds_src() { # the profile in THIS checkout behind arm fastdds@<F>
    case "$1" in
        F0) echo "$REPO/examples/perf_hil/fastdds/fastdds_eth0_only.xml" ;;
        mms1472) echo "$REPO/examples/perf_hil/fastdds/fastdds_eth0_only_mms1472.xml" ;;
        *) echo "$REPO/examples/perf_hil/fastdds/fastdds_keepall_$1.xml" ;;
    esac
}

# The launcher on each Pi: perf_test detached, its PID written by itself just before exec (so it is perf_test's own,
# never found by a pattern), and 2 s later (before a refused write can end it) the rmw libraries its /proc/PID/maps shows.
# shellcheck disable=SC2016 # expanded on the Pi, not here
LAUNCHER='#!/bin/bash
role=$1; envfile=$2; shift 2
source "$envfile"
PT=${KA_PERF_TEST:-$HOME/rmw_perf_ws/install/performance_test/lib/performance_test/perf_test}
rm -f /tmp/ka_$role.log /tmp/ka_$role.maps /tmp/ka_$role.pid /tmp/ka_$role.treat /tmp/ka_$role.start
date +%s%N > /tmp/ka_$role.start
setsid nohup bash -c "echo \$\$ > /tmp/ka_$role.pid; exec \"\$0\" \"\$@\"" "$PT" "$@" > /tmp/ka_$role.log 2>&1 < /dev/null &
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s /tmp/ka_$role.pid ] && break; sleep 0.2; done
p=$(cat /tmp/ka_$role.pid)
# The treatment as perf_test received it: the exported middleware variables (what exec hands on), and the profile
# file they name with its hash, read now and again from the process itself at 2 s if it is still alive.
treat() { grep -E "^(RMW_|FASTRTPS_|FASTDDS_|CYCLONEDDS_URI|ROS_DOMAIN_ID|SKIP_DEFAULT_XML_FILE)" | sort
    f=$(sed -n "s/^FASTRTPS_DEFAULT_PROFILES_FILE=//p" "$1"); [ -n "$f" ] && echo "profile_sha256=$(sha256sum < "$f" | cut -c1-64) $f"; }
env > /tmp/ka_$role.envall; { echo "[launcher env]"; treat /tmp/ka_$role.envall < /tmp/ka_$role.envall; } > /tmp/ka_$role.treat
( sleep 2; grep -o "/[^ ]*librmw_[a-z_]*\.so" /proc/$p/maps 2>/dev/null | sort -u > /tmp/ka_$role.maps
  if tr "\0" "\n" < /proc/$p/environ > /tmp/ka_$role.envproc 2>/dev/null && [ -s /tmp/ka_$role.envproc ]; then
    echo "[/proc/$p/environ]"; treat /tmp/ka_$role.envproc < /tmp/ka_$role.envproc
  else echo "[/proc/$p/environ] unreadable: perf_test ended before 2 s"; fi >> /tmp/ka_$role.treat
  rm -f /tmp/ka_$role.envall /tmp/ka_$role.envproc ) > /dev/null 2>&1 < /dev/null &
'
CELL_TOPICS=$(for c in $CELLS; do echo "${c%%:*}"; done | awk '!seen[$0]++')

# ---- EQUAL_BOUND: N per topic and each framework's bound, worked out here before anything runs (header) -----------
declare -A EQ_N=() EQ_BYTES=()
kv() { grep -oE "(^| )$1=[0-9]+" <<<"$2" | head -1 | cut -d= -f2; } # $1 key, $2 key=value line
if [ -n "$EQUAL_BOUND" ]; then
    say "--- EQUAL_BOUND $EQUAL_BOUND: bounds per topic, from $CLIENT's build (typesupport, rmw_tickle.h, core) ---"
    command -v xmllint > /dev/null || { say "REFUSED: no xmllint on this PC to validate the Fast DDS profile"; exit 1; }
    TS_DIR=/home/ci/rmw_perf_ws/build/performance_test/rosidl_typesupport_tickle_c/performance_test/msg
    RT_H=/home/ci/tickle/rmw_tickle/rmw_tickle/include/rmw_tickle_c/rmw_tickle.h
    XSD="$OUT.runs/fastRTPS_profiles.xsd"
    scp -q -i "$K" -o BatchMode=yes "ci@$CLIENT:/opt/ros/jazzy/share/fastRTPS_profiles.xsd" "$XSD" \
        || { say "REFUSED: cannot fetch the rig's fastRTPS_profiles.xsd"; exit 1; }
    sh_ "$CLIENT" "cat > /tmp/ka_bound.c" < "$REPO/examples/perf_hil/experiments/rmw_keepall_bound.c" \
        || { say "bound helper copy failed"; exit 1; }
    hdr=$(sh_ "$CLIENT" "grep -E '^#define RMW_TICKLE_(PSN_BYTES|PSN_SHORT_BYTES|TRACKING_WORDS) [0-9]+\$' $RT_H" </dev/null)
    psn=$(awk '$2 == "RMW_TICKLE_PSN_BYTES" {print $3}' <<<"$hdr")
    psn_short=$(awk '$2 == "RMW_TICKLE_PSN_SHORT_BYTES" {print $3}' <<<"$hdr")
    words=$(awk '$2 == "RMW_TICKLE_TRACKING_WORDS" {print $3}' <<<"$hdr")
    if [ -z "$psn" ] || [ -z "$psn_short" ] || [ -z "$words" ]; then
        say "REFUSED: $RT_H on $CLIENT does not define the psn sizes and tracking words as expected:"; say "$hdr"; exit 1
    fi
    for t in $CELL_TOPICS; do
        f="$TS_DIR/performance_test__msg__${t}__type_support.c"
        ts_c=$(sh_ "$CLIENT" "grep -hoE 'tickle_max_(encoded_size|buffer_length) = [0-9]+' $f" </dev/null | sort | tr '\n' ' ')
        ts_s=$(sh_ "$SERVER" "grep -hoE 'tickle_max_(encoded_size|buffer_length) = [0-9]+' $f" </dev/null | sort | tr '\n' ' ')
        enc=$(grep -oE 'encoded_size = [0-9]+' <<<"$ts_c" | grep -oE '[0-9]+$')
        buf=$(grep -oE 'buffer_length = [0-9]+' <<<"$ts_c" | grep -oE '[0-9]+$')
        if [ -z "$enc" ] || [ -z "$buf" ] || [ "$ts_c" != "$ts_s" ]; then
            say "REFUSED: $t's typesupport on the two Pis: client [$ts_c] server [$ts_s] (an unbounded type has no N)"
            exit 1
        fi
        b=$(sh_ "$CLIENT" "gcc -I \$HOME/tickle/include -Dtt_MAX_BUFFER_LENGTH=$buf -o /tmp/ka_bound /tmp/ka_bound.c &&
            /tmp/ka_bound $enc $psn_short $psn $words" </dev/null)
        foot=$(kv footprint "$b"); win=$(kv window_samples "$b"); ring=$(kv depth_samples "$b")
        [ -n "$foot" ] && [ -n "$win" ] && [ -n "$ring" ] || { say "REFUSED: bound helper said [$b] for $t"; exit 1; }
        if [ "$EQUAL_BOUND" = auto ]; then
            n=$((524288 / foot)); [ "$n" -gt 2048 ] && n=2048 # campaign_sweep.sh: min(2048, 512 KiB / footprint)
            want=$(for e in $EXPECTED_N; do [ "${e%%:*}" = "$t" ] && echo "${e#*:}"; done)
            if [ -n "$want" ] && [ "$want" != "$n" ]; then
                say "REFUSED: $t computes N = $n but the documents quote $want (EXPECTED_N): [$b] encoded $enc"; exit 1
            fi
        else
            n=$EQUAL_BOUND
        fi
        if [ "$n" -lt 1 ] || [ "$n" -gt "$win" ] || [ "$n" -gt "$ring" ]; then
            say "REFUSED: $t N = $n is past rmw_tickle's count bounds (window $win, ring $ring samples): bytes would not bind"
            exit 1
        fi
        EQ_N[$t]=$n; EQ_BYTES[$t]=$(((n + 1) * foot))
        fsha=-
        if [[ " $ARMS " == *" fastdds "* ]]; then
            prof="$OUT.runs/profile_eq$n.xml"
            sed "s/@N@/$n/g" "$REPO/examples/perf_hil/fastdds/fastdds_keepall_equal.xml.template" > "$prof"
            xmllint --noout --schema "$XSD" "$prof" 2>&1 | tee -a "$SUM"
            [ "${PIPESTATUS[0]}" = 0 ] || { say "REFUSED: $prof does not validate against the rig's XSD"; exit 1; }
            fsha=$(sha256sum < "$prof" | cut -c1-64)
            for h in "${HOSTS[@]}"; do
                sh_ "$h" "cat > /tmp/ka_fdds_eq$n.xml" < "$prof" || { say "profile copy failed"; exit 1; }
                pi_sha=$(sh_ "$h" "sha256sum < /tmp/ka_fdds_eq$n.xml" </dev/null | cut -c1-64)
                [ "$pi_sha" = "$fsha" ] || { say "profile eq$n on $h hashes $pi_sha, not $fsha"; exit 1; }
            done
        fi
        line="BOUND topic=$t n=$n encoded=$enc $b tickle_bytes=${EQ_BYTES[$t]}"
        line="$line fastdds_profile=/tmp/ka_fdds_eq$n.xml fastdds_sha256=$fsha cyclonedds=default"
        echo "$line" >> "$OUT.runs/bounds.txt"
        say "  $line"
    done
    [ "$DRY" = 1 ] && cp "$OUT.runs/bounds.txt" "$OUT.dry/bounds.txt"
fi

for h in "${HOSTS[@]}"; do
    printf '%s' "$LAUNCHER" | sh_ "$h" "cat > /tmp/ka_launch.sh && chmod +x /tmp/ka_launch.sh" || { say "launcher copy failed"; exit 1; }
    for arm in $ARMS; do
        for t in $CELL_TOPICS; do
            env_for "$arm" "$t" | sh_ "$h" "cat > /tmp/ka_env_${arm/@/_}_$t.sh" || { say "env copy failed"; exit 1; }
        done
        case "$arm" in fastdds@*) ;; *) continue ;; esac
        F=${arm#fastdds@}; src=$(fdds_src "$F")
        [ -f "$src" ] || { say "no profile $src for $arm"; exit 1; }
        sh_ "$h" "cat > /tmp/ka_fdds_$F.xml" < "$src" || { say "profile copy failed"; exit 1; }
        local_sha=$(sha256sum < "$src" | cut -c1-64)
        pi_sha=$(sh_ "$h" "sha256sum < /tmp/ka_fdds_$F.xml" </dev/null | cut -c1-64)
        [ "$local_sha" = "$pi_sha" ] || { say "profile $F on $h hashes $pi_sha, not $local_sha"; exit 1; }
        cp "$src" "$OUT.runs/profile_$F.xml"
        if [ "$h" = "$CLIENT" ]; then
            echo "PROFILE $F sha256=$local_sha src=${src#"$REPO"/} pi=/tmp/ka_fdds_$F.xml" >> "$OUT.runs/profiles.txt"
            say "treatment $arm: ${src#"$REPO"/} sha256 $local_sha -> /tmp/ka_fdds_$F.xml on both Pis; env:" \
                "$(env_for "$arm" | grep -E '^(export|unset) ' | grep -v ROS_DOMAIN | tr '\n' ';')"
        fi
    done
done

# CLOCK_PROBE (header): the probe on both hosts, and the server's test-link address it is asked on.
SERVER_IP=""
if [ "$CLOCK_PROBE" = 1 ] && [ "$SAMEHOST" != 1 ]; then
    for h in "${HOSTS[@]}"; do
        sh_ "$h" "cat > /tmp/ka_probe.py" < "$REPO/examples/perf_hil/experiments/clock_offset_probe.py" \
            || { say "probe copy failed"; exit 1; }
    done
    SERVER_IP=$(sh_ "$SERVER" "ip -4 -o addr show dev eth0" </dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
    [ -n "$SERVER_IP" ] || { say "REFUSED: no IPv4 address on $SERVER's eth0 for the clock probe"; exit 1; }
    say "clock probe: $CLIENT -> $SERVER_IP:$PROBE_PORT over eth0, before and after every run"
fi
clock_probe() { # $1 file, $2 label: one PROBE line (clock_offset_probe.py query), or "PROBE ok=0 ..." when it failed
    [ -n "$SERVER_IP" ] || return 0
    sh_ "$SERVER" "setsid nohup python3 /tmp/ka_probe.py serve --bind $SERVER_IP --port $PROBE_PORT --idle 3 --max 20 \
        > /tmp/ka_probe_srv.log 2>&1 < /dev/null &" </dev/null
    sleep 0.5
    local line
    line=$(sh_ "$CLIENT" "python3 /tmp/ka_probe.py query --server $SERVER_IP --port $PROBE_PORT --count 64" </dev/null)
    echo "$2 ${line:-PROBE ok=0 no output}" >> "$1"
}

launch() { # host role arm topic args...
    local h=$1 role=$2 arm=$3 topic=$4; shift 4
    sh_ "$h" "/tmp/ka_launch.sh $role /tmp/ka_env_${arm/@/_}_$topic.sh $*" </dev/null
}
wait_done() { # $1 host, $2 role, $3 deadline seconds
    sh_ "$1" "p=\$(cat /tmp/ka_$2.pid); for i in \$(seq 1 $3); do [ -d /proc/\$p ] || exit 0; sleep 1; done;
        case \"\$(readlink /proc/\$p/exe)\" in */perf_test) kill -INT \$p;; esac; sleep 2; echo killed" </dev/null
}

run_one() { # arm topic loss rep [runs dir, default $OUT.runs] [publisher seconds, default $DUR]
    local arm=$1 topic=$2 loss=$3 rep=$4 stem="$1_$2_l$3_r$4" dir=${5:-$OUT.runs} dur=${6:-$DUR}
    local common="-c ROS2 -t $topic $QOS_ARGS --dds_domain_id $DOMAIN"
    clock_probe "$dir/${stem}_clock.txt" before
    # Every arm, not only rmw_tickle's: rclcpp's type-description service needs a typesupport rmw_tickle does not
    # provide (its first run here died on it), and switching off an introspection service changes no data path.
    local rosargs="--ros-args --param start_type_description_service:=false"
    launch "$SERVER" sub "$arm" "$topic" "$common -p 0 -s 1 --expected_num_pubs 1 --max_runtime $((dur + 8)) $rosargs"
    sleep 2
    launch "$CLIENT" pub "$arm" "$topic" "$common -r $PUB_RATE -p 1 -s 0 --expected_num_subs 1 --max_runtime $dur $rosargs"
    local k1 k2
    k1=$(wait_done "$CLIENT" pub $((dur + 40)))
    k2=$(wait_done "$SERVER" sub 20)
    clock_probe "$dir/${stem}_clock.txt" after
    fetch_ "$CLIENT" /tmp/ka_pub.log "$dir/${stem}_pub.log"
    fetch_ "$SERVER" /tmp/ka_sub.log "$dir/${stem}_sub.log"
    fetch_ "$CLIENT" /tmp/ka_pub.start "$dir/${stem}_pub.start"
    fetch_ "$SERVER" /tmp/ka_sub.start "$dir/${stem}_sub.start"
    sh_ "$CLIENT" "cat /tmp/ka_pub.treat 2>/dev/null" </dev/null > "$dir/${stem}_pub.treat"
    sh_ "$SERVER" "cat /tmp/ka_sub.treat 2>/dev/null" </dev/null > "$dir/${stem}_sub.treat"
    local mp ms
    mp=$(sh_ "$CLIENT" "cat /tmp/ka_pub.maps 2>/dev/null" </dev/null | tr '\n' ' ')
    ms=$(sh_ "$SERVER" "cat /tmp/ka_sub.maps 2>/dev/null" </dev/null | tr '\n' ' ')
    echo "RUN $stem pub_maps=[$mp] sub_maps=[$ms] pub_killed=${k1:-no} sub_killed=${k2:-no} want=$(lib_for "$arm")" \
        >> "$dir/index.txt"
}

# ---- dry run, same lock (DRY=1; the default under EQUAL_BOUND): every arm once per topic, DRY_DUR s at 0% loss, read
# by the summary in --dry mode; any VOID - a treatment the publisher did not receive, a profile Fast DDS rejected, a
# writer that never delivered - stops the job before the real runs.
if [ "$DRY" = 1 ]; then
    set_loss 0
    say "--- dry run: every arm, every topic, ${DRY_DUR}s, 0% loss ---"
    for t in $CELL_TOPICS; do
        for arm in $ARMS; do run_one "$arm" "$t" 0 1 "$OUT.dry" "$DRY_DUR"; done
    done
    python3 "$SUMMARY" "$OUT.dry" "$DRY_DUR" --dry | tee -a "$SUM"
    if [ "${PIPESTATUS[0]}" != 0 ]; then say "DRY RUN FAILED - not running (runs in $OUT.dry)"; exit 1; fi
    say "--- dry run clean ---"
fi

cell_losses=$(for c in $CELLS; do echo "${c#*:}"; done | awk '!seen[$0]++')
for loss in $cell_losses; do
    set_loss "$loss" || { say "tc failed for $loss%"; exit 1; }
    say "--- loss $loss% ($(sh_ "$CLIENT" "tc qdisc show dev eth0" </dev/null | head -1)) ---"
    for topic in $(for c in $CELLS; do [ "${c#*:}" = "$loss" ] && echo "${c%%:*}"; done); do
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

python3 "$SUMMARY" "$OUT.runs" "$DUR" | tee -a "$SUM"
