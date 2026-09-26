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
# WAITS="poll block" (2026-09-26, 087c52d6): the ping's --wait mode, interleaved per round of rmws. poll (spin_some
# plus a 100 us sleep, RTT read after the loop) is the ping's default and what every earlier rmw row used; block
# waits in spin_once and reads the RTT in the callback, as the native client does. Dev measured poll - block at
# ~230-255 us for every rmw on veth (rmw_ping_wait_mode.sh). With the default "poll" the ping is invoked exactly
# as before, with no --wait flag; otherwise every row asserts the RESULT line's wait= matches the arm.
WAITS=${WAITS:-poll}
# SYSSTAMP_ARMS="off on" (2026-09-26, RMW_PERF_PLAN.md section 8, the user's request to measure the path out to the
# kernel and in from it for every rmw): "on" runs ping and pong under experiments/sysstamp (LD_PRELOAD), which
# records every socket and wait system call's entry and return on CLOCK_REALTIME, per process, into
# $OUT.sysstamp/<stem>_<role>/. rmw_pcap_split.py joins those with the pcaps (use with CAPTURE=1). "off" rows
# are the control: the layer's own cost is the on - off RTT difference, pre-registered below.
SYSSTAMP_ARMS=${SYSSTAMP_ARMS:-off}
# STAMPS=1 (2026-09-26, fdba1d22 on): ping and pong write per-sample application stamps (--stamps), collected into
# $OUT.stamps/<stem>_{ping,pong}.txt - the ping's reply_ns and the pong's callback and publish-return times, which
# split the application ends of each path. Needs a build that has --stamps.
STAMPS=${STAMPS:-0}
DOMAIN=${DOMAIN:-73}
OUT=${OUT:-/tmp/rmw_crosshost_rtt_$(date +%Y%m%d-%H%M%S).txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }
# TICKLE_VARIANTS (2026-09-26, rmw_tickle/RMW_PERF_PLAN.md H1-H3): extra rmw_tickle builds measured in the same
# session, interleaved with the other rmw implementations, e.g. "default trace trace_rxb8". Each non-default
# variant is built into ~/rmw_variants/<name>/ - outside ~/tickle, where `git clean` cannot reach it - and
# overlays only librmw_tickle.so; the typesupport and ping/pong are the default build's. Every rmw_tickle pong
# writes RMW_TICKLE_TRACE_FILE at shutdown: the node lock counters in any build (H1), the latency stamps in
# a trace build (H2), copied to $OUT.dumps/.
TICKLE_VARIANTS=${TICKLE_VARIANTS:-}
variant_args() {
    case "$1" in
        trace) echo "-DRMW_TICKLE_TRACE=ON" ;;
        trace_rxb8) echo "-DRMW_TICKLE_TRACE=ON -DRMW_TICKLE_RX_BATCH=8" ;;
        rxb8) echo "-DRMW_TICKLE_RX_BATCH=8" ;;
        *) echo "" ;;
    esac
}
VBUILD=""
for v in $TICKLE_VARIANTS; do
    [ "$v" = default ] && continue
    VBUILD="$VBUILD
colcon build --base-paths \$HOME/tickle --build-base \$HOME/rmw_variants/$v/build --install-base \$HOME/rmw_variants/$v/install \
  --packages-select rmw_tickle --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release $(variant_args "$v") > /tmp/rmwx_build_$v.log 2>&1 \
  || { echo \"VARIANT $v BUILD FAILED on \$(hostname)\"; tail -20 /tmp/rmwx_build_$v.log; exit 1; }
test -f \$HOME/rmw_variants/$v/install/rmw_tickle/lib/librmw_tickle.so"
done
TRACE=${TRACE:-0}
if [ "$TRACE" = 1 ]; then REPS=1; MSGS=bench; fi
say "=== rmw cross-host RTT, $(date -Is), SHA $SHA, $REPS reps, msgs: $MSGS, spin arms: ${SPIN_ARMS:-off}, trace: $TRACE, waits: $WAITS, sysstamp arms: $SYSSTAMP_ARMS ==="

CDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>'
FDDS_PROFILE=/home/ci/tickle/examples/perf_hil/fastdds/fastdds_eth0_only.xml
ENV_BASE="set +u; source /opt/ros/jazzy/setup.bash; [ -f \$HOME/rmw_perf_ws/install/setup.bash ] && source \$HOME/rmw_perf_ws/install/setup.bash; source \$HOME/tickle/install/setup.bash; set -u; export ROS_DOMAIN_ID=$DOMAIN"
env_for() {
    local base="${1%@*}" v="default"
    case "$1" in *@*) v="${1#*@}" ;; esac
    if [ "$base" = rmw_tickle ] && [ "$v" != default ]; then
        echo "$(env_for rmw_tickle); set +u; source \$HOME/rmw_variants/$v/install/setup.bash; set -u"
        return
    fi
    case "$base" in
        rmw_tickle) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
        rmw_fastrtps_cpp) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTRTPS_DEFAULT_PROFILES_FILE=$FDDS_PROFILE" ;;
        rmw_cyclonedds_cpp) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp CYCLONEDDS_URI='$CDDS_URI'" ;;
    esac
}
lib_for() {
    case "$1" in
        rmw_tickle@default) echo "/home/ci/tickle/install/rmw_tickle/lib/librmw_tickle.so"; return ;;
        rmw_tickle@*) echo "/home/ci/rmw_variants/${1#*@}/install/rmw_tickle/lib/librmw_tickle.so"; return ;;
    esac
    case "$1" in
        rmw_tickle) echo "/home/ci/tickle/install/rmw_tickle/lib/librmw_tickle.so" ;;
        rmw_fastrtps_cpp) echo "/librmw_fastrtps_cpp.so" ;;
        rmw_cyclonedds_cpp) echo "/librmw_cyclonedds_cpp.so" ;;
    esac
}

# TRACE=1 (2026-09-26): one repetition of bench/best_effort per rmw, with the pong under strace -f -tt -T,
# the traces copied to $OUT.traces/. For decomposing where each rmw spends a round trip, not for timing:
# strace slows every syscall, so its RTTs are never reported as figures. SKIP_BUILD=1 reuses the rig's
# build only if both Pis are at exactly $SHA and the binaries exist.
if [ "${SKIP_BUILD:-0}" = 1 ]; then
    for h in "$CLIENT" "$SERVER"; do
        at=$(sh_ "$h" "git -C \$HOME/tickle rev-parse HEAD; test -x \$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong/pong_node && echo bin-ok")
        case "$at" in "$SHA"*bin-ok*) ;; *) say "SKIP_BUILD refused: $h is not at $SHA with binaries built"; exit 1 ;; esac
    done
    say "--- SKIP_BUILD: both rpis already at $SHA with binaries ---"
else
say "--- building rmw_tickle + rmw_perf_pingpong (Release) at $SHA on both rpis ---"
pids=()
for h in "$CLIENT" "$SERVER"; do
    # Three stages, in the order the rig's own provision_rmw_perf_ws.sh established (2026-09-26: a
    # one-shot colcon call failed, because ~/rmw_perf_ws's builtin_interfaces/rcl_interfaces were built
    # WITH TickLE's typesupport and so need it installed first, and colcon started rmw_perf_pingpong
    # before the typesupport had finished). Stage 2 regenerates those two packages' TickLE bindings
    # with the current generator rather than trusting ones generated days ago - a stale generated
    # binding has measured the wrong thing in this repository before.
    sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
# The TickLE typesupport generator's Python package, straight from this checkout rather than pip-installed:
# the rig's system Python refuses pip (PEP 668), and an installed copy goes stale while the source moves
# on - which has already made a regression check here test the wrong code.
export PYTHONPATH=\$HOME/tickle/tools/typesupport\${PYTHONPATH:+:\$PYTHONPATH}
set +u; source /opt/ros/jazzy/setup.bash; set -u
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
  --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release > /tmp/rmwx_build1.log 2>&1 \
  || { echo \"STAGE 1 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/rmwx_build1.log; exit 1; }
set +u; source \$HOME/tickle/install/setup.bash; set -u
colcon build --base-paths \$HOME/rmw_perf_ws --build-base \$HOME/rmw_perf_ws/build --install-base \$HOME/rmw_perf_ws/install \
  --packages-select builtin_interfaces rcl_interfaces --cmake-args -DCMAKE_BUILD_TYPE=Release --cmake-force-configure > /tmp/rmwx_build2.log 2>&1 \
  || { echo \"STAGE 2 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/rmwx_build2.log; exit 1; }
set +u; source \$HOME/rmw_perf_ws/install/setup.bash; source \$HOME/tickle/install/setup.bash; set -u
colcon build --packages-select rmw_perf_pingpong --cmake-args -DCMAKE_BUILD_TYPE=Release > /tmp/rmwx_build3.log 2>&1 \
  || { echo \"STAGE 3 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/rmwx_build3.log; exit 1; }
test -f \$HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so
test -x \$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong/ping_node
test -x \$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong/pong_node
rm -rf \$HOME/rmw_variants
$VBUILD
echo \"built on \$(hostname) at \$(git rev-parse --short HEAD)\"" 2>&1 | tee -a "$OUT" &
    pids+=($!)
done
bad=0; for p in "${pids[@]}"; do wait "$p" || bad=1; done
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "test -x \$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong/pong_node" || bad=1
done
[ "$bad" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }
fi
case " $SYSSTAMP_ARMS " in *" on "*)
    for h in "$CLIENT" "$SERVER"; do
        scp -q -i "$K" -o BatchMode=yes "$REPO/examples/perf_hil/experiments/sysstamp/sysstamp.c" "ci@$h:/tmp/rmwx_sysstamp.c"
        sh_ "$h" "cc -O2 -shared -fPIC -o /tmp/rmwx_libsysstamp.so /tmp/rmwx_sysstamp.c -ldl" \
            || { say "sysstamp build FAILED on $h - not running"; exit 1; }
    done
    say "--- sysstamp built on both rpis from source md5 $(md5sum "$REPO/examples/perf_hil/experiments/sysstamp/sysstamp.c" | cut -c1-12) ---" ;;
esac

# SPIN_ARMS="off on" (2026-09-26): repeat every repetition with and without a nice-19 spinner on core 0 of
# both Pis, which holds the one cpufreq policy (cores 0-3) at its maximum while cores 1-3 stay idle
# between pings - the method of rtt_spinner_arm.sh. Asks whether rmw_tickle's ~0.08 ms deficit to
# CycloneDDS is DVFS: rmw_tickle's pong runs 4 threads and CycloneDDS's 8, so they may hold the clock
# differently under the ondemand governor. Pre-registered, before running:
#   - with the spinner, both Pis must read their maximum scaling_cur_freq, or the "on" arm is VOID;
#   - gap = median(rmw_tickle) - median(rmw_cyclonedds). gap_on <= gap_off / 2: DVFS is most of it,
#     a platform property rather than something in rmw_tickle's code. |gap_on - gap_off| <= 0.020 ms:
#     not DVFS, and the next step is in-process timestamps through rmw_tickle's receive path.
# Spinners are started with their PIDs captured at launch and stopped by those PIDs.
SPIN_ARMS=${SPIN_ARMS:-off}
declare -A SPIN_PID=()
spin_on() {
    local h
    for h in "$CLIENT" "$SERVER"; do
        SPIN_PID[$h]=$(sh_ "$h" "setsid nohup nice -n 19 taskset -c 0 sh -c 'while :; do :; done' >/dev/null 2>&1 < /dev/null & echo \$!")
    done
    sleep 2
}
spin_off() {
    local h
    for h in "${!SPIN_PID[@]}"; do
        sh_ "$h" "[ -d /proc/${SPIN_PID[$h]} ] && tr '\\0' ' ' < /proc/${SPIN_PID[$h]}/cmdline | grep -q 'while :; do :; done' && kill ${SPIN_PID[$h]}" >/dev/null 2>&1 || true
        unset "SPIN_PID[$h]"
    done
    sleep 1
}
trap spin_off EXIT
freqs() { echo "$(sh_ "$CLIENT" 'cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq') $(sh_ "$SERVER" 'cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq')"; }

# CAPTURE=1 (2026-09-26, RMW_PERF_PLAN.md section 6): tcpdump on both Pis for every run, so a round trip can be
# split into ping side, wire and pong side on one clock per host, for every rmw alike - no stamps needed. The
# capture ends itself (-G/-W: no kill needed, and sudo here allows tcpdump but not kill). On both hosts the
# CLOCK_REALTIME - CLOCK_MONOTONIC offset is sampled around the run: pcap times are REALTIME and the ping's
# send_ns is MONOTONIC, so the client's offset puts the ping's own send stack on one clock, and the server's
# aligns pcap times with rx_wake stamps. The snap length keeps the whole Bench sample (send_ns, seq), which
# rmw_pcap_split.py matches packets by - 128 bytes could cut it off behind a longer RTPS header.
# CAP_S covers 1 s lead, the pong's 4 s, discovery and the ping's 10 s, with room to spare.
CAPTURE=${CAPTURE:-0}
CAP_S=30
cap_start() { # $1 file stem. Each run writes its own file, owned by ci (-Z ci), and removes it once copied.
    # 2026-09-26: with one shared name and tcpdump's default drop to user tcpdump, ci could neither remove the
    # file nor, under fs.protected_regular=2, let the next capture reopen it ("Permission denied"). Every run
    # after the first then copied the same stale file. The start time on each host goes into $OUT so that
    # rmw_pcap_split.py can refuse a capture that began before its own run.
    local h role
    # -U writes each packet as it comes: without it the first copy of a capture was a 40,960-byte prefix of the
    # file tcpdump went on to finish, because the copy is taken while tcpdump still holds a buffer.
    CAP_FILE=/tmp/rmwx_cap_$1_$(date +%s).pcap
    for h in "$CLIENT" "$SERVER"; do
        role=ping; [ "$h" = "$SERVER" ] && role=pong
        say "  capture stem=$1 role=$role t0_ns=$(sh_ "$h" "date +%s%N")"
        sh_ "$h" "sudo -n tcpdump -U -Z ci -i eth0 -n -s 256 --time-stamp-precision=nano -G $CAP_S -W 1 -w $CAP_FILE udp > /tmp/rmwx_tcpdump.log 2>&1 < /dev/null &"
    done
    sleep 1
}
cap_collect() { # $1 file stem
    local h role
    sleep $((CAP_S + 1))
    mkdir -p "$OUT.pcaps"
    for h in "$CLIENT" "$SERVER"; do
        role=ping; [ "$h" = "$SERVER" ] && role=pong
        scp -q -i "$K" -o BatchMode=yes "ci@$h:$CAP_FILE" "$OUT.pcaps/${1}_${role}.pcap" 2>/dev/null || say "  (no pcap from $role)"
        sh_ "$h" "rm -f $CAP_FILE" || true
    done
}
clock_offset() { # prints client=<ns> server=<ns>, REALTIME - MONOTONIC on each host
    local h role
    for h in "$CLIENT" "$SERVER"; do
        role=client; [ "$h" = "$SERVER" ] && role=server
        printf '%s=%s ' "$role" "$(sh_ "$h" "python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_REALTIME)-time.clock_gettime_ns(time.CLOCK_MONOTONIC))'")"
    done
}

BIN=/home/ci/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong
one() { # $1 rmw, $2 msg, $3 qos (best_effort|reliable), $4 rep
    local rmw="$1" msg="$2" qos="$3" rep="$4" flag="" env pongpid maps res verdict=ok
    [ "$qos" = reliable ] && flag="--reliable"
    env=$(env_for "$rmw")
    local pre=""
    [ "$TRACE" = 1 ] && pre="strace -f -tt -T -o /tmp/rmwx_trace_$rmw.txt"
    local off0="" stem="${rmw}_${msg}_${qos}_rep${rep}${WAITSTEM}${SSTSTEM}" sstenv="" stampflag=""
    local stamprm=""
    # removed before each start too, so an interrupted row's file can never be collected as this row's
    [ "$STAMPS" = 1 ] && stampflag="--stamps /tmp/rmwx_stamps.txt" && stamprm="rm -f /tmp/rmwx_stamps.txt;"
    if [ "$SST" = on ]; then
        sstenv="rm -f /tmp/rmwx_sst.*; export LD_PRELOAD=/tmp/rmwx_libsysstamp.so SYSSTAMP_FILE=/tmp/rmwx_sst;"
    fi
    if [ "$CAPTURE" = 1 ]; then cap_start "$stem"; off0=$(clock_offset); fi
    local dumpenv=""
    case "$rmw" in rmw_tickle*) dumpenv="export RMW_TICKLE_TRACE_FILE=/tmp/rmwx_dump.txt; rm -f /tmp/rmwx_dump.txt;" ;; esac
    pongpid=$(sh_ "$SERVER" "$env; $dumpenv $sstenv $stamprm nohup taskset -c 1-3 $pre $BIN/pong_node $flag $stampflag -m $msg > /tmp/rmwx_pong.log 2>&1 < /dev/null & echo \$!")
    sleep 4
    if [ "$TRACE" = 1 ]; then
        # $! is strace; the pong is its child, found by parentage and verified by /proc/PID/exe, not by name
        pongpid=$(sh_ "$SERVER" "for c in \$(cat /proc/$pongpid/task/$pongpid/children 2>/dev/null); do [ \"\$(readlink /proc/\$c/exe)\" = $BIN/pong_node ] && echo \$c; done" | head -1)
    fi
    maps=$(sh_ "$SERVER" "grep -o '/[^ ]*librmw_[a-z_]*\.so' /proc/$pongpid/maps 2>/dev/null | sort -u | tr '\n' ' '" || true)
    # CPU and peak memory, measured from outside and identically for all three (2026-09-26): the ping is
    # wrapped in /usr/bin/time, the pong's /proc/PID/stat and /status are read just before it is stopped.
    # The pong's figures cover its whole life (4 s idle before the ping starts, 10 s of pings), so they are
    # per-process totals, not per-message costs, and compare only between rmw implementations run alike.
    local waitflag=""
    [ "$WAITS" != poll ] && waitflag="--wait $WAIT"
    # How many ping/pong processes are alive on both Pis as the ping starts (2026-09-26), found by /proc/PID/exe,
    # never by name. More than one of either means a leftover from an earlier row is answering or announcing
    # too: every rmw would see extra peers, and rmw_tickle would broadcast above tt_UNICAST_PEER_THRESHOLD.
    local procs
    procs="pong_procs=$(sh_ "$SERVER" "n=0; for q in /proc/[0-9]*; do [ \"\$(readlink \$q/exe 2>/dev/null)\" = $BIN/pong_node ] && n=\$((n+1)); done; echo \$n") ping_procs_before=$(sh_ "$CLIENT" "n=0; for q in /proc/[0-9]*; do [ \"\$(readlink \$q/exe 2>/dev/null)\" = $BIN/ping_node ] && n=\$((n+1)); done; echo \$n")"
    # LOOP: (274c7b8e on, both wait modes) is the ping's own loop accounting - iterations_per_rtt, and in poll mode
    # spin_some_us and sleep_us - kept in the row after RESULT's fields, whose order it does not change.
    # The ping's own log (stderr) is kept per row with the pong's in $OUT.logs, for questions such as which peers
    # rmw_tickle's publisher registered.
    res=$(sh_ "$CLIENT" "$env; $sstenv $stamprm timeout 60 /usr/bin/time -f 'ping_utime_s=%U ping_stime_s=%S ping_maxrss_kb=%M' -o /tmp/rmwx_ping_time.txt taskset -c 1-3 $BIN/ping_node -i 0.1 -d 10 $flag $waitflag $stampflag -m $msg 2>/tmp/rmwx_ping.log; cat /tmp/rmwx_ping_time.txt" | grep -E '^RESULT:|^LOOP:|^ping_utime_s' | tr '\n' ' ' || true)
    res="$res $procs"
    local pongcpu
    # pong_cpu_ns: summed run time of every pong thread from /proc/PID/task/*/schedstat (ns), because the
    # tick-based utime+stime (pong_cpu_s, 10 ms granularity) cannot separate rmw_tickle from CycloneDDS.
    pongcpu=$(sh_ "$SERVER" "[ -d /proc/$pongpid ] && awk -v t=\$(getconf CLK_TCK) '{printf \"pong_cpu_s=%.2f \", (\$14+\$15)/t}' /proc/$pongpid/stat && cat /proc/$pongpid/task/*/schedstat | awk '{s+=\$1} END{printf \"pong_cpu_ns=%d \", s}' && awk '/VmHWM/{printf \"pong_maxrss_kb=%s\", \$2}' /proc/$pongpid/status" 2>/dev/null || true)
    res="$res $pongcpu"
    sh_ "$SERVER" "[ -d /proc/$pongpid ] && [ \"\$(readlink /proc/$pongpid/exe)\" = $BIN/pong_node ] && kill -INT $pongpid" >/dev/null 2>&1 || true
    sleep 1
    if [ "$SST" = on ] || [ "$STAMPS" = 1 ]; then
        # the pong writes its sysstamp records and its stamps as it exits, so wait for it to be gone (up to 5 s)
        sh_ "$SERVER" "for i in 1 2 3 4 5 6 7 8 9 10; do [ -d /proc/$pongpid ] || break; sleep 0.5; done" || true
    fi
    if [ "$STAMPS" = 1 ]; then
        local sh sr
        mkdir -p "$OUT.stamps"
        for sh in "$CLIENT" "$SERVER"; do
            sr=ping; [ "$sh" = "$SERVER" ] && sr=pong
            scp -q -i "$K" -o BatchMode=yes "ci@$sh:/tmp/rmwx_stamps.txt" "$OUT.stamps/${stem}_${sr}.txt" 2>/dev/null \
                || say "  (no stamps from $sr)"
            sh_ "$sh" "rm -f /tmp/rmwx_stamps.txt" || true
        done
    fi
    if [ "$SST" = on ]; then
        local h role
        for h in "$CLIENT" "$SERVER"; do
            role=ping; [ "$h" = "$SERVER" ] && role=pong
            mkdir -p "$OUT.sysstamp/${stem}_${role}"
            scp -q -i "$K" -o BatchMode=yes "ci@$h:/tmp/rmwx_sst.*" "$OUT.sysstamp/${stem}_${role}/" 2>/dev/null \
                || say "  (no sysstamp records from $role)"
        done
    fi
    mkdir -p "$OUT.logs"
    scp -q -i "$K" -o BatchMode=yes "ci@$CLIENT:/tmp/rmwx_ping.log" "$OUT.logs/${stem}_ping.log" 2>/dev/null || say "  (no ping log)"
    scp -q -i "$K" -o BatchMode=yes "ci@$SERVER:/tmp/rmwx_pong.log" "$OUT.logs/${stem}_pong.log" 2>/dev/null || say "  (no pong log)"
    if [ "$CAPTURE" = 1 ]; then
        local off1; off1=$(clock_offset)
        say "  clock_offset_ns stem=$stem before: ${off0}after: ${off1}"
        cap_collect "$stem"
    fi
    case "$rmw" in rmw_tickle*)
        sleep 1; mkdir -p "$OUT.dumps"
        scp -q -i "$K" -o BatchMode=yes "ci@$SERVER:/tmp/rmwx_dump.txt" "$OUT.dumps/${rmw}_${msg}_${qos}_rep${rep}.txt" 2>/dev/null \
            || say "  (no dump from $rmw rep$rep)" ;;
    esac
    case "$maps" in *"$(lib_for "$rmw")"*) ;; *) verdict="VOID(pong loaded: ${maps:-nothing})" ;; esac
    case "$res" in *"framework=${rmw%@*} "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(no RESULT for $rmw)" ;; esac
    case "$res" in *"loss_pct=0 "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(loss)" ;; esac
    if [ "$WAITS" != poll ]; then
        case "$res" in *" wait=$WAIT "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(ping did not run wait=$WAIT)" ;; esac
    fi
    case "${ARM_TAG:-}" in *VOID-freq*) [ "$verdict" = ok ] && verdict="VOID(spinner did not lift the clock)" ;; esac
    say "$rmw $msg $qos rep$rep${WAITSTEM:+ wait=$WAIT}${SSTSTEM:+ sst=on}${ARM_TAG:-} | $verdict | ${res#RESULT: }"
    if [ "$TRACE" = 1 ]; then
        sleep 1; mkdir -p "$OUT.traces"
        scp -q -i "$K" -o BatchMode=yes "ci@$SERVER:/tmp/rmwx_trace_$rmw.txt" "$OUT.traces/$rmw.txt" || say "  (trace copy failed for $rmw)"
    fi
}
for rep in $(seq 1 "$REPS"); do
  for spin in $SPIN_ARMS; do
    if [ "$spin" = on ]; then spin_on; else spin_off; fi
    ARM_TAG=""
    if [ "$SPIN_ARMS" != off ]; then
        f=$(freqs); ARM_TAG=" spin=$spin freq_khz=${f// /,}"
        if [ "$spin" = on ]; then
            maxf=$(sh_ "$CLIENT" 'cat /sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq')
            case "$f" in "$maxf $maxf") ;; *) ARM_TAG="$ARM_TAG VOID-freq" ;; esac
        fi
    fi
    for msg in $MSGS; do
        qoses="best_effort reliable"; [ "$TRACE" = 1 ] && qoses=best_effort
        for qos in $qoses; do
            RMWS="rmw_tickle rmw_fastrtps_cpp rmw_cyclonedds_cpp"
            if [ -n "$TICKLE_VARIANTS" ]; then
                RMWS=""; for v in $TICKLE_VARIANTS; do RMWS="$RMWS rmw_tickle@$v"; done
                RMWS="$RMWS rmw_fastrtps_cpp rmw_cyclonedds_cpp"
            fi
            for WAIT in $WAITS; do
              WAITSTEM=""; [ "$WAITS" != poll ] && WAITSTEM="_$WAIT"
              for SST in $SYSSTAMP_ARMS; do
                SSTSTEM=""; [ "$SST" = on ] && SSTSTEM="_sst"
                for rmw in $RMWS; do
                  one "$rmw" "$msg" "$qos" "$rep"
                done
              done
            done
        done
    done
  done
done
say ""; say "=== done ==="
