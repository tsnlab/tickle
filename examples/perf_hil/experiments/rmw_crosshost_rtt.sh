#!/usr/bin/env bash
# Cross-host rmw-layer round trip: rmw_tickle against rmw_fastrtps_cpp, rmw_cyclonedds_cpp and rmw_zenoh_cpp, each
# reached the way a ROS 2 application reaches it (rclcpp, RMW_IMPLEMENTATION), ping on rpi#1 and pong
# on rpi#2 (rmw_tickle/rmw_perf_pingpong). Re-measures COMPARISON.md's rmw rows, the only rows where
# TickLE is not first, on the current core. The previous figures (0.524/0.523 ms against FastDDS
# 0.516/0.523 and CycloneDDS 0.440/0.449, 2026-09-21) predate the release build of both core and
# rmw_tickle, the scheduler-driven poll, sendmmsg and DATA_FRAG.
#
# Fairness, enforced rather than assumed (COMPARISON.md 4.4):
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
#
# GATE V2 (the user's decision of 2026-10-11; gate_v2_lib.sh, gate_v2.py, TESTING.md 6.1):
#   A1  every TickLE unit this script builds is layout-stable: TICKLE_EXTRA_CFLAGS (default TICKLE_LAYOUT_CFLAGS,
#       -falign-functions=64 + NOPAD, when unset; set and empty = the plain build) is appended to CMAKE_C_FLAGS and
#       CMAKE_CXX_FLAGS of all three colcon stages - rmw_tickle with the TickLE core it compiles into its .so (tickle.c,
#       encoding.c, log.c, hal_linux.c) and the two TickLE typesupports; builtin_interfaces and rcl_interfaces, whose
#       TickLE typesupport libraries compile encoding.c and log.c in; rmw_perf_pingpong, whose own messages do the same.
#       Every stage is configured with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON, each build's compile_commands.json is copied
#       back to $OUT.build_<host>_<build>.json and read by rmw_build_flags_check.py: every unit must carry every flag
#       word (last -falign-functions= wins), and the units named there must be present. One "build flags verified:" line
#       per Pi and build (the builds are listed on a "--- builds to verify: ... ---" line), else nothing is measured.
#       Each Pi's gcc version and its default -falign-* values are recorded beside them. Also with SKIP_BUILD=1.
#   A2  INSN_COUNT=1 (the default when RMW_LIST is rmw_tickle alone and TICKLE_VARIANTS is empty; refused with any other
#       rmw): ping and pong each run under `perf stat -x, -e instructions:u,instructions:k` (insn_prefix), and the row
#       gets ping_insn_u= ping_insn_k= ping_insn=ok and pong_insn_u= pong_insn_k= pong_insn=ok (insn_fields), or
#       <role>_insn=fail:<why>, which makes the row VOID(insn...). Counted only when the run is rmw_tickle-only: the
#       instrument costs a PMU save/restore per context switch, and the vendors run more threads and switch more, so
#       counting every rmw "identically" would not be neutral (TESTING.md section 5, item 4) - the published
#       cross-vendor rows stay uncounted. Each process is counted over its whole life (start-up, discovery, the idle
#       wait, every round trip, shutdown); the reader divides by the ping's sent= (every round trip). A launch's $! is
#       then perf's: the pong's PID used for its maps, CPU, idle reading and SIGINT is perf's child whose /proc exe is
#       pong_node (gate_v2_lib.sh child_exe), the ping's is found by parentage below timeout and time; after the pong
#       is stopped the script waits for perf itself to exit, so its counts are written. Not with TRACE=1 or a sysstamp
#       arm (strace would wrap perf; LD_PRELOAD would load into perf as well).
#   A3  this script measures one SHA and one flag set per invocation; rmw_gate_v2_chain.sh runs it as the mirrored
#       blocks A P B B P A and rmw_gate_v2.py reads them by gate v2.
# ROS_SETUP (default /opt/ros/jazzy/setup.bash) and PI_HOME (default /home/ci, the Pis' home as an absolute path, which the
# identity checks compare /proc/PID/maps against): only for rig_dryrun.sh on a PC with another distro and other homes.
set -uo pipefail
# shellcheck source=examples/perf_hil/experiments/pi_checkout.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$(dirname "${BASH_SOURCE[0]}")/pi_checkout.sh"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$REPO/examples/perf_hil/experiments/gate_v2_lib.sh"
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
# POLL_SLEEPS="0 50 100 200" (2026-09-26, 27f75dd2 on): the poll-mode sleep between spins, as a sweep. One value
# is an arbitrary grid: with 100 us rmw_tickle's cheaper spin_some() makes it catch the reply a cycle later
# (RMW_PERF_PLAN 8.1). Unset, the ping is invoked exactly as before (no --poll-sleep-us, i.e. 100). Set, each
# poll row passes --poll-sleep-us N, is labelled poll_sleep_us=N, and asserts the LOOP line reports that N.
POLL_SLEEPS=${POLL_SLEEPS:-}
# BPF_ARMS="off on" (2026-09-26, RMW_PERF_PLAN 8.2): "on" attaches experiments/pong_rx_split.bt to the pong under
# sudo bpftrace (granted by the user that day), one line per ping: the kernel receive path from the NIC IRQ to the
# receive syscall's return, and the handoff to the thread that sends the reply. Collected into $OUT.bpf/<stem>.txt.
# "off" rows are the control for bpftrace's own cost (on - off RTT, block mode).
BPF_ARMS=${BPF_ARMS:-off}
# BPF_SCRIPT (2026-10-31, c12/idle_wake_bpf_rig.sh): the bpftrace script of the "on" arm, called the same way (pong tgid,
# eth0 IRQ) and checked by the same "# pong_rx_split" first line. Unset: pong_rx_split.bt, as before.
BPF_SCRIPT=${BPF_SCRIPT:-$REPO/examples/perf_hil/experiments/pong_rx_split.bt}
# EXEC_POLL_ARMS="off on" (2026-09-26, Dev's d2c1e36f): rmw_tickle's executor-driven receive, switched by
# RMW_TICKLE_EXECUTOR_POLL=0|1 on both ping and pong (the same binaries in both arms). Each rmw_tickle row asserts
# the ping's own shutdown line "rmw_tickle: executor_poll=<0|1>" matches its arm; the vendors run in the off arm
# only. RMW_LIST replaces the three rmws, e.g. RMW_LIST=rmw_tickle for an rmw_tickle-only A/B.
EXEC_POLL_ARMS=${EXEC_POLL_ARMS:--}
# PHASE / RTT_AT (2026-09-27, RMW_PERF_PLAN.md 10.2, the user's decision): the ping's --phase and --rtt-at in poll
# mode. PHASE=jitter puts a random pause of up to one poll cycle between each publish and its first check, so the reply
# lands at a random phase of the cycle instead of the same phase every time; RTT_AT=callback ends the round trip at
# the reply's callback, as block mode does, instead of after that poll cycle's sleep. Unset, the ping runs as before
# (locked, loop). Set, every poll row asserts the ping's PHASE: line shows them, and a jitter row is VOID unless its
# placement chi2 against uniform over 10 bins is below 21.67 (9 degrees of freedom, p = 0.01).
PHASE=${PHASE:-}
RTT_AT=${RTT_AT:-}
RMW_LIST=${RMW_LIST:-rmw_tickle rmw_fastrtps_cpp rmw_cyclonedds_cpp}
# QOSES (2026-10-10, ~/rig_queue_largemsg_A4.sh): the QoS cells, best_effort and/or reliable. Unset, both, in that
# order, exactly as before; QOSES=best_effort re-runs one cell without the other.
QOSES=${QOSES:-"best_effort reliable"}
for q in $QOSES; do
    case "$q" in best_effort | reliable) ;; *) echo "QOSES takes best_effort and/or reliable, not '$q'" >&2; exit 64 ;; esac
done
# IDLE_S=10 (2026-09-27, RMW_PERF_PLAN 8.6's idle-CPU criterion): the pong idles IDLE_S s before the ping starts
# instead of 4, and the row gains pong_idle_cpu_ns / pong_idle_ms - the summed schedstat run time of every pong
# thread from 1 s after launch (start-up excluded) to the ping's start, and that window's length on the Pi's clock.
# Unset, the pong idles 4 s exactly as before and the row has no idle fields.
IDLE_S=${IDLE_S:-}
# INJECT_ARMS="none 1 50" (2026-10-31, c12/idle_wake_rig.sh): per row, c12/idle_wake_injector.py runs on the ping host and
# sends one empty UDP datagram to the pong's data socket LEAD ms before each predicted ping ("none": the same injector,
# predicting and sending nothing - the control). It reads the ping host's capture, so it needs CAPTURE=1. Each row gets
# inject=<arm> and the injector's INJECT_END fields; a row whose injector printed no INJECT_END, or whose sent count
# contradicts its arm (none with any send, a lead with fewer than 50), is VOID(inject ...). Whether the datagram reached
# and woke the pong's receiving thread is read on the pong side (c12/idle_wake_read.py). Arms rotate per rep with
# ROTATE=1. Unset ("-"): no injector runs and every stem and row is as before.
INJECT_ARMS=${INJECT_ARMS:--}
# VARIANT_VENDORS (2026-10-31): the vendor rmws appended after the rmw_tickle@<variant> list when TICKLE_VARIANTS is set.
# Unset, both, exactly as before.
VARIANT_VENDORS=${VARIANT_VENDORS-rmw_fastrtps_cpp rmw_cyclonedds_cpp}
# COUNT_RMWS / COUNT_REPS (2026-10-31, c12/step4_rig.sh): in reps 1..COUNT_REPS, each inject arm is run once more for every
# rmw in COUNT_RMWS with counters on its receiving thread (stem suffix _cnt): perf stat -t of dTLB-load-misses,
# L1-dcache-load-misses, instructions and cycles, and c12/migr.bt (sched_switch cpu changes, sched_migrate_task) under
# sudo bpftrace, both for COUNT_S s from just before the ping. The thread: rmw_tickle's main thread (its receive and its
# reply), CycloneDDS's task named exactly recvUC (else recv); every task's name is kept beside the counts. Unset: none.
COUNT_RMWS=${COUNT_RMWS:-}
# PONG_CPUS / WARM_ARMS (2026-10-31, c12/step5_rig.sh): PONG_CPUS replaces the pong's taskset list (default 1-3, as
# before). WARM_ARMS="off same other": per row, c12/warmer.c (WARM_S s, 5 us of spinning every 10 ms) runs on the pong's
# host pinned to WARM_CPU_SAME or WARM_CPU_OTHER, started just before the ping ("off": none); only rmws in WARM_RMWS get
# the warmer arms, the others run "off" alone. Every warmer row keeps the warmer's WARMER_END line, and with WARM_ARMS
# every row records each pong task's last cpu and the process's Cpus_allowed_list after the ping ($OUT.pin/). Unset
# ("-"): no warmer, no pin record, stems as before.
PONG_CPUS=${PONG_CPUS:-1-3}
# PROF=1 (2026-10-31, c12/step6_rig.sh): every row samples its receiving thread (rmw_tickle's main thread, CycloneDDS's
# task named exactly recvUC, else recv; every task's name kept) with c12/step6_prof.bt under sudo bpftrace, cycles every
# 10000, kernel and user symbols, for PROF_S s from just before the ping, into $OUT.prof/<stem>.txt. Unset: none.
PROF=${PROF:-0}
PROF_S=${PROF_S:-15}
WARM_ARMS=${WARM_ARMS:--}
WARM_RMWS=${WARM_RMWS:-}
WARM_CPU_SAME=${WARM_CPU_SAME:-2}
WARM_CPU_OTHER=${WARM_CPU_OTHER:-3}
WARM_S=${WARM_S:-16}
WARMSTEM=""
WARM=off
COUNT_REPS=${COUNT_REPS:-0}
COUNT_S=${COUNT_S:-14}
CNTSTEM=""
if [ "$INJECT_ARMS" != - ] && [ "${CAPTURE:-0}" != 1 ]; then
    echo "REFUSED: INJECT_ARMS needs CAPTURE=1 (the injector predicts the pings from the ping host's capture)" >&2
    exit 64
fi
for a in $INJECT_ARMS; do
    case "$a" in - | none | [0-9] | [0-9][0-9] | [0-9].[0-9]) ;; *) echo "INJECT_ARMS takes none or a lead in ms, not '$a'" >&2; exit 64 ;; esac
done
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
        # libc (2026-10-21): the HAL's hot system calls through glibc instead of raw tt_sys6 (hal_linux.c), so that
        # sysstamp's LD_PRELOAD sees rmw_tickle's sends, receives and epoll_pwait2 as it sees the vendors'. The last
        # -DCMAKE_C_FLAGS wins over RMW_TICKLE_BUILD's, so the flags are repeated with the define added.
        # libc_rxthread (2026-10-31, c12/rxthread_rig.sh): the libc build, run with RMW_TICKLE_EXECUTOR_POLL=0 (env_for).
        libc | libc_rxthread) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -DTT_HAL_LIBC_CALLS'" ;;
        # libc_ringbase, libc_ringff, libc_ringpf (2026-11-01, c12/kh2b_rig.sh, with PI_PATCH=c12/kh2b.patch): the wait
        # ring's recvmsg as on main, through a registered file, or issued poll-first (TT_RX_RING_FIXED_FILE,
        # TT_RX_RING_POLL_FIRST).
        libc_ringbase) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -DTT_HAL_LIBC_CALLS -DTT_RX_RING_FIXED_FILE=0 -DTT_RX_RING_POLL_FIRST=0'" ;;
        libc_ringff) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -DTT_HAL_LIBC_CALLS -DTT_RX_RING_POLL_FIRST=0'" ;;
        libc_ringpf) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -DTT_HAL_LIBC_CALLS -DTT_RX_RING_FIXED_FILE=0'" ;;
        # ringoff, libc_ringoff (2026-10-22, rig_queue_rx_uring_wait.sh): the HAL's wait ring compiled off by default
        # (tt_RX_URING_WAIT 0: tt_receive() waits with epoll and reads with recvfrom()), the control of a build whose
        # default has it on.
        ringoff) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -Dtt_RX_URING_WAIT=0'" ;;
        libc_ringoff) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -DTT_HAL_LIBC_CALLS -Dtt_RX_URING_WAIT=0'" ;;
        # libc_noslack, libc_far (2026-10-31, c12/step2_rig.sh): the polling thread's timer slack left at the kernel's
        # default (TT_RX_TIMER_SLACK_NS 0), and every timed wait made 10 s long with no deadline timer
        # (TT_EXP_WAIT_FAR_NS, an experiment-only flag that exists only with PI_PATCH=c12/wait_far.patch).
        libc_noslack) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -DTT_HAL_LIBC_CALLS -DTT_RX_TIMER_SLACK_NS=0'" ;;
        libc_far) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -DTT_HAL_LIBC_CALLS -DTT_EXP_WAIT_FAR_NS=10000000000LL'" ;;
        # libc_rb, libc_rb_csum, libc_rb_nogro (2026-10-31, c12/step3_rig.sh, with PI_PATCH=c12/step3.patch): the
        # sockets' options read back with getsockopt() at bind and close (TT_EXP_C12_READBACK); _csum runs with
        # TT_UDP_CSUM0=0 (env_for), _nogro never touches UDP_GRO (TT_EXP_NO_GRO_SETUP).
        libc_rb | libc_rb_csum | libc_rb_nolazy) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -DTT_HAL_LIBC_CALLS -DTT_EXP_C12_READBACK'" ;;
        libc_rb_nogro) echo "'-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS -DTT_HAL_LIBC_CALLS -DTT_EXP_C12_READBACK -DTT_EXP_NO_GRO_SETUP'" ;;
        *) echo "" ;;
    esac
}
# rmw_tickle is built the way the vendor rmws it is compared with were (fairness audit, 2026-10-05): the jazzy debs
# export their CMake targets as "-none" - CMAKE_BUILD_TYPE None, Debian's -O2, no NDEBUG - while this built rmw_tickle
# as Release, -O3 -DNDEBUG. -g -O2 is the level they share; the packages' hardening and LTO flags are not repeated,
# since stripped binaries do not say which the build farm used.
# Gate v2 A1 (header): the layout flags go on the end of every stage's C and C++ flags.
layout_cflags_default
case "$TICKLE_EXTRA_CFLAGS" in *[!A-Za-z0-9=_\ -]*)
    echo "TICKLE_EXTRA_CFLAGS may hold only [A-Za-z0-9=_ -]: '$TICKLE_EXTRA_CFLAGS'" >&2; exit 64 ;; esac
LAYOUT_BUILD="'-DCMAKE_C_FLAGS=$TICKLE_EXTRA_CFLAGS' '-DCMAKE_CXX_FLAGS=$TICKLE_EXTRA_CFLAGS' -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
RMW_TICKLE_BUILD="-DCMAKE_BUILD_TYPE=None '-DCMAKE_C_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS' '-DCMAKE_CXX_FLAGS=-g -O2 $TICKLE_EXTRA_CFLAGS' -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
ROS_SETUP=${ROS_SETUP:-/opt/ros/jazzy/setup.bash}
PI_HOME=${PI_HOME:-/home/ci}
# Gate v2 A2 (header): counted only when rmw_tickle runs alone.
if [ "$RMW_LIST" = rmw_tickle ] && [ -z "$TICKLE_VARIANTS" ]; then INSN_COUNT=${INSN_COUNT:-1}; else INSN_COUNT=${INSN_COUNT:-0}; fi
case "$INSN_COUNT" in 0 | 1) ;; *) echo "INSN_COUNT must be 0 or 1" >&2; exit 64 ;; esac
if [ "$INSN_COUNT" = 1 ]; then
    if [ "$RMW_LIST" != rmw_tickle ] || [ -n "$TICKLE_VARIANTS" ]; then
        echo "REFUSED: INSN_COUNT=1 needs RMW_LIST=rmw_tickle and no TICKLE_VARIANTS (no rmw runs under an instrument the others do not)" >&2
        exit 64
    fi
    case " ${TRACE:-0} $SYSSTAMP_ARMS " in *" 1 "* | *" on "*)
        echo "REFUSED: INSN_COUNT=1 with TRACE=1 or a sysstamp arm (strace would wrap perf; LD_PRELOAD would load into perf)" >&2
        exit 64 ;;
    esac
fi
VBUILD=""
for v in $TICKLE_VARIANTS; do
    [ "$v" = default ] && continue
    VBUILD="$VBUILD
colcon build --base-paths \$HOME/tickle --build-base \$HOME/rmw_variants/$v/build --install-base \$HOME/rmw_variants/$v/install \
  --packages-select rmw_tickle --cmake-args -DBUILD_SHARED_LIBS=ON $RMW_TICKLE_BUILD $(variant_args "$v") > /tmp/rmwx_build_$v.log 2>&1 \
  || { echo \"VARIANT $v BUILD FAILED on \$(hostname)\"; tail -20 /tmp/rmwx_build_$v.log; exit 1; }
test -f \$HOME/rmw_variants/$v/install/rmw_tickle/lib/librmw_tickle.so"
done
TRACE=${TRACE:-0}
if [ "$TRACE" = 1 ]; then REPS=1; MSGS=bench; fi
# PI_PATCH (2026-10-31, c12/step2_rig.sh): a patch from this checkout applied with `git apply` on both Pis right after the
# checkout, before every build of the session (so every build, default and variants, is of the same patched tree). It
# must only add code behind a flag that is off by default. The next run's checkout (git reset --hard) removes it. Unset:
# nothing applied, as before.
PI_PATCH=${PI_PATCH:-}
PATCH_APPLY=":"
if [ -n "$PI_PATCH" ]; then
    [ -f "$PI_PATCH" ] || { echo "PI_PATCH $PI_PATCH: no such file" >&2; exit 64; }
    PATCH_APPLY="git apply /tmp/rmwx_pi.patch || { echo \"PATCH FAILED on \$(hostname)\"; exit 1; }"
fi
say "=== rmw cross-host RTT, $(date -Is), SHA $SHA, $REPS reps, msgs: $MSGS, spin arms: ${SPIN_ARMS:-off}, trace: $TRACE, waits: $WAITS, poll sleeps: ${POLL_SLEEPS:-default}, sysstamp arms: $SYSSTAMP_ARMS, bpf arms: $BPF_ARMS, executor-poll arms: $EXEC_POLL_ARMS, phase: ${PHASE:-locked}, rtt at: ${RTT_AT:-loop}, idle: ${IDLE_S:-4} s, inject arms: $INJECT_ARMS, rmws: $RMW_LIST (rotated per rep: ${ROTATE:-0}), qoses: $QOSES ==="
say "TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS' (every rmw_tickle, TickLE typesupport, interface and pingpong unit; empty: the plain build)"
say "INSN_COUNT=$INSN_COUNT (perf stat instructions:u/k on the ping and the pong; 1 only with RMW_LIST=rmw_tickle)"

CDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>'
FDDS_PROFILE=$PI_HOME/tickle/examples/perf_hil/fastdds/fastdds_eth0_only.xml
ENV_BASE="set +u; source $ROS_SETUP; [ -f \$HOME/rmw_perf_ws/install/setup.bash ] && source \$HOME/rmw_perf_ws/install/setup.bash; source \$HOME/tickle/install/setup.bash; set -u; export ROS_DOMAIN_ID=$DOMAIN"
env_for() {
    local base="${1%@*}" v="default"
    case "$1" in *@*) v="${1#*@}" ;; esac
    if [ "$base" = rmw_tickle ] && [ "$v" != default ]; then
        local extra=""
        [ "$v" = libc_rb_csum ] && extra="; export TT_UDP_CSUM0=0" # c12 step 3 (b): every checksum kept
        [ "$v" = libc_rb_nolazy ] && extra="; export TT_RX_LAZY_TIMER=0" # c12 step 4 (d): no deadline timerfd in the set
        # c12 experiment (a): every receive on rmw_tickle's own poll thread, the executor waiting on wait_cond
        [ "$v" = libc_rxthread ] && extra="; export RMW_TICKLE_EXECUTOR_POLL=0"
        echo "$(env_for rmw_tickle); set +u; source \$HOME/rmw_variants/$v/install/setup.bash; set -u$extra"
        return
    fi
    case "$base" in
        rmw_tickle) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255" ;;
        rmw_fastrtps_cpp) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTRTPS_DEFAULT_PROFILES_FILE=$FDDS_PROFILE" ;;
        rmw_cyclonedds_cpp) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp CYCLONEDDS_URI='$CDDS_URI'" ;;
        # Added 2026-09-30 at the user's instruction. No router daemon: rmw_zenoh_cpp's shipped session config is
        # mode "peer" with autoconnect to both routers and peers and connect_scouted true, so two peers on this L2
        # segment find each other by multicast scouting - the same way the other two rmws run here, with no extra
        # process. `rmw_zenohd` exists in the package if that turns out to be wrong; a session that does not form
        # shows up as a run with no pong rather than as a number, which is what makes trying it without one safe.
        rmw_zenoh_cpp) echo "$ENV_BASE; export RMW_IMPLEMENTATION=rmw_zenoh_cpp ZENOH_SESSION_CONFIG_URI=$ZENOH_SESSION_CFG" ;;
    esac
}
lib_for() {
    case "$1" in
        rmw_tickle@default) echo "$PI_HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so"; return ;;
        rmw_tickle@*) echo "$PI_HOME/rmw_variants/${1#*@}/install/rmw_tickle/lib/librmw_tickle.so"; return ;;
    esac
    case "$1" in
        rmw_tickle) echo "$PI_HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so" ;;
        rmw_fastrtps_cpp) echo "/librmw_fastrtps_cpp.so" ;;
        rmw_cyclonedds_cpp) echo "/librmw_cyclonedds_cpp.so" ;;
        rmw_zenoh_cpp) echo "/librmw_zenoh_cpp.so" ;;
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
if [ -n "$PI_PATCH" ]; then
    for h in "$CLIENT" "$SERVER"; do
        scp -q -i "$K" -o BatchMode=yes "$PI_PATCH" "ci@$h:/tmp/rmwx_pi.patch" || { say "PI_PATCH copy FAILED - not running"; exit 1; }
    done
    say "--- PI_PATCH $(basename "$PI_PATCH") md5 $(md5sum "$PI_PATCH" | cut -c1-12), applied after the checkout on both rpis ---"
fi
pids=()
for h in "$CLIENT" "$SERVER"; do
    # Three stages, in the order the rig's own provision_rmw_perf_ws.sh established (2026-09-26: a
    # one-shot colcon call failed, because ~/rmw_perf_ws's builtin_interfaces/rcl_interfaces were built
    # WITH TickLE's typesupport and so need it installed first, and colcon started rmw_perf_pingpong
    # before the typesupport had finished). Stage 2 regenerates those two packages' TickLE bindings
    # with the current generator rather than trusting ones generated days ago - a stale generated
    # binding has measured the wrong thing in this repository before.
    sh_ "$h" "set -e
cd ~/tickle && $(pi_checkout "$SHA") && git clean -fdqx -e install -e build -e log
$PATCH_APPLY
# The TickLE typesupport generator's Python package, straight from this checkout rather than pip-installed:
# the rig's system Python refuses pip (PEP 668), and an installed copy goes stale while the source moves
# on - which has already made a regression check here test the wrong code.
export PYTHONPATH=\$HOME/tickle/tools/typesupport\${PYTHONPATH:+:\$PYTHONPATH}
set +u; source $ROS_SETUP; set -u
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
  --cmake-args -DBUILD_SHARED_LIBS=ON $RMW_TICKLE_BUILD > /tmp/rmwx_build1.log 2>&1 \
  || { echo \"STAGE 1 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/rmwx_build1.log; exit 1; }
set +u; source \$HOME/tickle/install/setup.bash; set -u
colcon build --base-paths \$HOME/rmw_perf_ws --build-base \$HOME/rmw_perf_ws/build --install-base \$HOME/rmw_perf_ws/install \
  --packages-select builtin_interfaces rcl_interfaces --cmake-args -DCMAKE_BUILD_TYPE=Release $LAYOUT_BUILD --cmake-force-configure > /tmp/rmwx_build2.log 2>&1 \
  || { echo \"STAGE 2 BUILD FAILED on \$(hostname)\"; tail -25 /tmp/rmwx_build2.log; exit 1; }
set +u; source \$HOME/rmw_perf_ws/install/setup.bash; source \$HOME/tickle/install/setup.bash; set -u
colcon build --packages-select rmw_perf_pingpong --cmake-args -DCMAKE_BUILD_TYPE=Release $LAYOUT_BUILD > /tmp/rmwx_build3.log 2>&1 \
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
# Gate v2 A1 (header): each build's own record of what its compiler was given, read back here from its
# compile_commands.json - not from the variable. Any build that does not show TICKLE_EXTRA_CFLAGS on every unit, or lacks
# a unit it must have compiled (needs_of), stops the run before anything is measured. Recorded with them: each Pi's gcc
# and its default alignments, and how many of librmw_tickle.so's text symbols the realized binary has 64- and
# 128-byte aligned (information only; start-up symbols from the crt objects are not ours and are not aligned).
BUILDS="rmw_tickle:tickle/build/rmw_tickle ts_c:tickle/build/rosidl_typesupport_tickle_c ts_cpp:tickle/build/rosidl_typesupport_tickle_cpp"
BUILDS="$BUILDS builtin_interfaces:rmw_perf_ws/build/builtin_interfaces rcl_interfaces:rmw_perf_ws/build/rcl_interfaces"
BUILDS="$BUILDS rmw_perf_pingpong:tickle/build/rmw_perf_pingpong"
for v in $TICKLE_VARIANTS; do [ "$v" = default ] || BUILDS="$BUILDS variant_$v:rmw_variants/$v/build/rmw_tickle"; done
needs_of() {
    case "$1" in
    rmw_tickle | variant_*) echo "--need /rmw_tickle/src/rmw_node.c --need /src/tickle.c --need /src/encoding.c --need /src/log.c --need /src/hal_linux.c" ;;
    ts_c | ts_cpp) echo "--need /src/identifier.c" ;;
    builtin_interfaces | rcl_interfaces) echo "--need /src/encoding.c --need /src/log.c" ;;
    rmw_perf_pingpong) echo "--need /src/ping_node.cpp --need /src/pong_node.cpp --need /src/encoding.c" ;;
    esac
}
say "--- builds to verify: $(for b in $BUILDS; do printf '%s ' "${b%%:*}"; done)---"
for h in "$CLIENT" "$SERVER"; do
    # shellcheck disable=SC2016 # expanded on the Pi
    say "compiler on $h: $(sh_ "$h" 'gcc --version | head -1; gcc -O2 -Q --help=optimizers | grep -E "falign-(functions|loops|jumps)=" | tr -s " \t" " "' 2>&1 | tr '\n' ';')"
    for b in $BUILDS; do
        name=${b%%:*}
        json="$OUT.build_${h##*.}_$name.json"
        sh_ "$h" "cat \$HOME/${b#*:}/compile_commands.json" >"$json" 2>/dev/null || true
        # shellcheck disable=SC2046 # needs_of's words are separate arguments on purpose
        if ! verified=$(python3 "$REPO/examples/perf_hil/experiments/rmw_build_flags_check.py" "$json" "$TICKLE_EXTRA_CFLAGS" $(needs_of "$name")); then
            say "FATAL build flags: $h $name does not show TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS': $verified ($json). Not running."
            exit 1
        fi
        say "build flags verified: $h $name TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS' ($verified)"
    done
    # The address's last two hex digits decide both (no strtonum: the Pis' awk may be mawk).
    # shellcheck disable=SC2016 # expanded on the Pi
    say "librmw_tickle.so on $h: $(sh_ "$h" 'nm --defined-only $HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so | awk '\''$2 ~ /^[tT]$/ { n++; t = tolower(substr($1, length($1) - 1)); if (t ~ /^[048c]0$/) a64++; if (t ~ /^[08]0$/) a128++ } END { printf "%d text symbols, %d 64-byte aligned, %d 128-byte aligned", n, a64, a128 }'\''' 2>&1)"
done
# One application, the rmw chosen at run time (2026-09-27, the user's question): ping_node and pong_node are built
# once per session and run unchanged for every rmw; RMW_IMPLEMENTATION alone selects the rmw, which
# rmw_implementation loads dynamically. Recorded here: each binary's sha256 on both Pis, and that neither links any
# rmw implementation itself (ldd shows the generic librmw.so / librmw_implementation.so only). A binary linking an
# implementation directly stops the run.
for h in "$CLIENT" "$SERVER"; do
    ident=$(sh_ "$h" "cd $PI_HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong && sha256sum ping_node pong_node | awk '{printf \"%s=%s \", \$2, substr(\$1,1,16)}'; for b in ping_node pong_node; do ldd \$b | grep -oE 'librmw_(tickle|fastrtps_cpp|cyclonedds_cpp|fastrtps_shared_cpp|dds_common)[^ ]*' | sed \"s/^/\$b-links-/\"; done" 2>&1)
    say "--- binaries on $h: $ident ---"
    case "$ident" in *-links-*) say "an application binary links an rmw implementation directly - not running"; exit 1 ;; esac
done
if [ "$WARM_ARMS" != - ]; then
    if ! scp -q -i "$K" -o BatchMode=yes "$REPO/examples/perf_hil/experiments/c12/warmer.c" "ci@$SERVER:/tmp/c12_warmer.c" ||
        ! sh_ "$SERVER" "cc -O2 -o /tmp/c12_warmer /tmp/c12_warmer.c"; then
        say "warmer build FAILED - not running"
        exit 1
    fi
    say "--- warmer: c12/warmer.c md5 $(md5sum "$REPO/examples/perf_hil/experiments/c12/warmer.c" | cut -c1-12), arms $WARM_ARMS for $WARM_RMWS, same cpu $WARM_CPU_SAME, other cpu $WARM_CPU_OTHER, pong cpus $PONG_CPUS ---"
fi
if [ "$PROF" = 1 ]; then
    scp -q -i "$K" -o BatchMode=yes "$REPO/examples/perf_hil/experiments/c12/step6_prof.bt" "ci@$SERVER:/tmp/c12_profile.bt" \
        || { say "step6_prof.bt copy FAILED - not running"; exit 1; }
    say "--- profile rows: c12/step6_prof.bt md5 $(md5sum "$REPO/examples/perf_hil/experiments/c12/step6_prof.bt" | cut -c1-12), $PROF_S s per row ---"
fi
if [ -n "$COUNT_RMWS" ]; then
    scp -q -i "$K" -o BatchMode=yes "$REPO/examples/perf_hil/experiments/c12/migr.bt" "ci@$SERVER:/tmp/c12_migr.bt" \
        || { say "migr.bt copy FAILED - not running"; exit 1; }
    say "--- counter rows: $COUNT_RMWS, reps 1..$COUNT_REPS, $COUNT_S s, migr.bt md5 $(md5sum "$REPO/examples/perf_hil/experiments/c12/migr.bt" | cut -c1-12) ---"
fi
if [ "$INJECT_ARMS" != - ]; then
    scp -q -i "$K" -o BatchMode=yes "$REPO/examples/perf_hil/experiments/c12/idle_wake_injector.py" "ci@$CLIENT:/tmp/c12_idle_wake_injector.py" \
        || { say "injector copy FAILED - not running"; exit 1; }
    say "--- idle-wake injector: c12/idle_wake_injector.py md5 $(md5sum "$REPO/examples/perf_hil/experiments/c12/idle_wake_injector.py" | cut -c1-12), arms: $INJECT_ARMS ---"
fi
BPF_IRQ=""
case " $BPF_ARMS " in *" on "*)
    scp -q -i "$K" -o BatchMode=yes "$BPF_SCRIPT" "ci@$SERVER:/tmp/rmwx_pong_rx_split.bt"
    BPF_IRQ=$(sh_ "$SERVER" "awk -F: '/eth0/ {gsub(/ /, \"\", \$1); print \$1}' /proc/interrupts")
    [ -n "$BPF_IRQ" ] || { say "bpftrace: no eth0 IRQ found on the pong host - not running"; exit 1; }
    say "--- bpftrace arm: $(basename "$BPF_SCRIPT") md5 $(md5sum "$BPF_SCRIPT" | cut -c1-12), eth0 IRQ $BPF_IRQ ---" ;;
esac
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
# rmw_zenoh_cpp needs a Zenoh router and says so at runtime: "Unable to connect to a Zenoh router. Have you started
# a router with `ros2 run rmw_zenoh_cpp rmw_zenohd`", then "timed out waiting for a match". Its shipped session config
# reads mode "peer" with autoconnect to routers and peers, which is why the first attempt was made without one - that
# was a behavioural question answered from a config file, and the run corrected it. One router on the pong host serves
# both nodes; they find it by the same multicast scouting the config describes.
ZENOHD_PID=""
ZENOH_ROUTER_CFG=${ZENOH_ROUTER_CFG:-/tmp/zenoh_router_eth0.json5}
ZENOH_SESSION_CFG=${ZENOH_SESSION_CFG:-/tmp/zenoh_session_eth0.json5}
zenohd_on() {
    case " $RMW_LIST " in *" rmw_zenoh_cpp "*) ;; *) return 0 ;; esac
    # `ros2 run` is a WRAPPER: the daemon it spawns is a child with a different pid, so $! names the wrapper and
    # killing it leaves the daemon holding tcp/7447. That happened on the first attempt - the next router could not
    # bind, and the /proc check had passed because the wrapper really was gone. setsid makes the wrapper a process
    # group leader, so the group id is the wrapper's pid and killing the GROUP takes the daemon with it.
    # The configs ship with the repo and are copied here, not left in /tmp by hand: a run whose config file is
    # missing gets the shipped default, which is the no-router peer mode that fails - and it would fail the same way
    # whether the file was absent or wrong, which is the kind of silent difference this harness exists to refuse.
    local cfgdir="$REPO/examples/perf_hil/zenohpico/rmw_config"
    local h
    for h in "$CLIENT" "$SERVER"; do
        scp -q -i "$K" -o BatchMode=yes "$cfgdir/zenoh_router_eth0.json5" "$cfgdir/zenoh_session_eth0.json5" "ci@$h:/tmp/" \
            || { say "FATAL could not copy the zenoh configs to $h"; return 1; }
    done
    ZENOHD_PID=$(sh_ "$SERVER" "$ENV_BASE; export ZENOH_ROUTER_CONFIG_URI=$ZENOH_ROUTER_CFG; setsid nohup ros2 run rmw_zenoh_cpp rmw_zenohd > /tmp/rmwx_zenohd.log 2>&1 < /dev/null & echo \$!")
    sleep 4
    # Verified by /proc/PID, not by assuming the launch worked: a router that failed to start would otherwise look
    # exactly like the no-router case this exists to remove, and every zenoh row would void for the same reason twice.
    # Checked on the LISTENER, not on the wrapper's pid: "the wrapper is alive" and "a router is accepting
    # connections" are different facts, and only the second is what the measurement needs.
    if ! sh_ "$SERVER" "ss -ltn 2>/dev/null | grep -q ':7447 '" >/dev/null 2>&1; then
        say "FATAL no router listening on $SERVER:7447 (wrapper pid ${ZENOHD_PID:-none}); zenoh rows would all VOID"
        sh_ "$SERVER" "tail -5 /tmp/rmwx_zenohd.log" 2>&1 | sed 's/^/  /' | tee -a "$OUT"
        return 1
    fi
    say "  rmw_zenohd up on $SERVER, pid $ZENOHD_PID"
}
zenohd_off() {
    [ -z "${ZENOHD_PID:-}" ] && return 0
    # The whole process group, and then verified by the port rather than by the pid.
    sh_ "$SERVER" "kill -TERM -- -$ZENOHD_PID 2>/dev/null; sleep 2; ss -ltn 2>/dev/null | grep -q ':7447 ' && { echo 'router still listening on 7447'; kill -KILL -- -$ZENOHD_PID 2>/dev/null; }" >/dev/null 2>&1 || true
    ZENOHD_PID=""
}
# ONE exit handler. spin_off had the trap; adding a second `trap ... EXIT` would silently replace it, which is how a
# netem restore was lost on 2026-09-29.
rmwx_on_exit() { spin_off; zenohd_off; }
trap rmwx_on_exit EXIT
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
    # INJECT_ARMS: the injector reads the ping host's capture as it grows, and without --immediate-mode libpcap hands
    # tcpdump its packets in blocks up to 1 s late (the first injection run sent 11 datagrams for 100 pings). The ping
    # host's capture only, in every arm alike; the pong host's capture, which the reading is taken from, is unchanged.
    local imm=""
    for h in "$CLIENT" "$SERVER"; do
        role=ping; [ "$h" = "$SERVER" ] && role=pong
        imm=""; [ "$INJECT_ARMS" != - ] && [ "$role" = ping ] && imm="--immediate-mode"
        say "  capture stem=$1 role=$role t0_ns=$(sh_ "$h" "date +%s%N")${imm:+ $imm}"
        sh_ "$h" "sudo -n tcpdump -U $imm -Z ci -i eth0 -n -s 256 --time-stamp-precision=nano -G $CAP_S -W 1 -w $CAP_FILE udp > /tmp/rmwx_tcpdump.log 2>&1 < /dev/null &"
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

BIN=$PI_HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong
one() { # $1 rmw, $2 msg, $3 qos (best_effort|reliable), $4 rep
    local rmw="$1" msg="$2" qos="$3" rep="$4" flag="" env pongpid maps res verdict=ok
    [ "$qos" = reliable ] && flag="--reliable"
    # PONG_THREADS=N (2026-10-22): the pong spins a MultiThreadedExecutor of N threads (pong_node --executor-threads);
    # every row says pong_threads=N. Unset: rclcpp::spin(), as before.
    local pongthreads=""
    [ -n "${PONG_THREADS:-}" ] && pongthreads="--executor-threads $PONG_THREADS"
    env=$(env_for "$rmw")
    case "$EP:$rmw" in
    on:rmw_tickle*) env="$env; export RMW_TICKLE_EXECUTOR_POLL=1" ;;
    off:rmw_tickle*) env="$env; export RMW_TICKLE_EXECUTOR_POLL=0" ;;
    esac
    local pre=""
    [ "$TRACE" = 1 ] && pre="strace -f -tt -T -o /tmp/rmwx_trace_$rmw.txt"
    local off0="" stem="${rmw}_${msg}_${qos}_rep${rep}${WAITSTEM}${PSLEEP:+_ps$PSLEEP}${SSTSTEM}${BPFSTEM}${EPSTEM}${INJSTEM}${CNTSTEM}${WARMSTEM}" sstenv="" stampflag=""
    local stamprm=""
    # removed before each start too, so an interrupted row's file can never be collected as this row's
    [ "$STAMPS" = 1 ] && stampflag="--stamps /tmp/rmwx_stamps.txt" && stamprm="rm -f /tmp/rmwx_stamps.txt;"
    if [ "$SST" = on ]; then
        sstenv="rm -f /tmp/rmwx_sst.*; export LD_PRELOAD=/tmp/rmwx_libsysstamp.so SYSSTAMP_FILE=/tmp/rmwx_sst;"
    fi
    if [ "$CAPTURE" = 1 ]; then cap_start "$stem"; off0=$(clock_offset); fi
    local dumpenv=""
    case "$rmw" in rmw_tickle*) dumpenv="export RMW_TICKLE_TRACE_FILE=/tmp/rmwx_dump.txt; rm -f /tmp/rmwx_dump.txt;" ;; esac
    # Gate v2 A2 (header): under perf stat the launch's $! is perf's; the pong is perf's child, found by /proc exe
    # (child_exe) and used for everything below - maps, CPU, idle, bpftrace, SIGINT. perf's own PID is kept to wait on.
    local pongperf="" pongrm="" pingrm="" pingpre="" insn_void=""
    if [ "$INSN_COUNT" = 1 ]; then
        pre=$(insn_prefix /tmp/rmwx_pong.insn)
        pingpre=$(insn_prefix /tmp/rmwx_ping.insn)
        pongrm="rm -f /tmp/rmwx_pong.insn;" pingrm="rm -f /tmp/rmwx_ping.insn;"
    fi
    pongpid=$(sh_ "$SERVER" "$env; $dumpenv $sstenv $stamprm $pongrm nohup taskset -c $PONG_CPUS $pre $BIN/pong_node $flag $pongthreads $stampflag -m $msg > /tmp/rmwx_pong.log 2>&1 < /dev/null & echo \$!")
    if [ "$INSN_COUNT" = 1 ]; then
        pongperf=$pongpid
        pongpid=$(sh_ "$SERVER" "$(remote_child_of_perf)
for i in \$(seq 1 30); do child_exe $pongperf '$BIN/pong_node' && exit 0; sleep 0.1; done; exit 1" || true)
        if [ -z "$pongpid" ]; then pongpid=$pongperf; insn_void="VOID(insn: no pong_node under perf $pongperf)"; fi
    fi
    local idle=""
    if [ -n "$IDLE_S" ] && [ "$TRACE" != 1 ]; then
        local rd="cat /proc/$pongpid/task/*/schedstat 2>/dev/null | awk -v t=\$(date +%s%N) '{s+=\$1} END{print s+0, t}'"
        sleep 1
        local i0 i1
        i0=$(sh_ "$SERVER" "$rd" || echo "0 0")
        sleep $((IDLE_S - 1))
        i1=$(sh_ "$SERVER" "$rd" || echo "0 0")
        idle=$(echo "$i0 $i1" | awk '$1 > 0 && $3 > 0 {printf "pong_idle_cpu_ns=%d pong_idle_ms=%.0f", $3 - $1, ($4 - $2) / 1e6}')
    else
        sleep 4
    fi
    if [ "$TRACE" = 1 ]; then
        # $! is strace; the pong is its child, found by parentage and verified by /proc/PID/exe, not by name
        pongpid=$(sh_ "$SERVER" "for c in \$(cat /proc/$pongpid/task/$pongpid/children 2>/dev/null); do [ \"\$(readlink /proc/\$c/exe)\" = $BIN/pong_node ] && echo \$c; done" | head -1)
    fi
    maps=$(sh_ "$SERVER" "grep -o '/[^ ]*librmw_[a-z_]*\.so' /proc/$pongpid/maps 2>/dev/null | sort -u | tr '\n' ' '" || true)
    local bpfpid=""
    if [ "$BPF" = on ]; then
        # timeout's own PID from its launch ($!): stopping it after the ping stops bpftrace through sudo. 40 s is
        # only the ceiling if this script dies; the ping takes ~12 s.
        bpfpid=$(sh_ "$SERVER" "rm -f /tmp/rmwx_bpf.txt; nohup timeout 40 sudo -n bpftrace /tmp/rmwx_pong_rx_split.bt $pongpid $BPF_IRQ > /tmp/rmwx_bpf.txt 2>&1 < /dev/null & echo \$!")
        sh_ "$SERVER" "for i in \$(seq 1 20); do grep -q '^# pong_rx_split' /tmp/rmwx_bpf.txt 2>/dev/null && break; sleep 0.5; done" || true
    fi
    # CPU and peak memory, measured from outside and identically for all three (2026-09-26): the ping is
    # wrapped in /usr/bin/time, the pong's /proc/PID/stat and /status are read just before it is stopped.
    # The pong's figures cover its whole life (4 s idle before the ping starts, 10 s of pings), so they are
    # per-process totals, not per-message costs, and compare only between rmw implementations run alike.
    local waitflag=""
    [ "$WAITS" != poll ] && waitflag="--wait $WAIT"
    [ -n "$PSLEEP" ] && waitflag="$waitflag --poll-sleep-us $PSLEEP"
    if [ "$WAIT" = poll ]; then
        [ -n "$PHASE" ] && waitflag="$waitflag --phase $PHASE"
        [ -n "$RTT_AT" ] && waitflag="$waitflag --rtt-at $RTT_AT"
    fi
    # How many ping/pong processes are alive on both Pis as the ping starts (2026-09-26), found by /proc/PID/exe,
    # never by name. More than one of either means a leftover from an earlier row is answering or announcing
    # too: every rmw would see extra peers, and rmw_tickle would broadcast above tt_UNICAST_PEER_THRESHOLD.
    local procs
    procs="pong_procs=$(sh_ "$SERVER" "n=0; for q in /proc/[0-9]*; do [ \"\$(readlink \$q/exe 2>/dev/null)\" = $BIN/pong_node ] && n=\$((n+1)); done; echo \$n") ping_procs_before=$(sh_ "$CLIENT" "n=0; for q in /proc/[0-9]*; do [ \"\$(readlink \$q/exe 2>/dev/null)\" = $BIN/ping_node ] && n=\$((n+1)); done; echo \$n")"
    # LOOP: (274c7b8e on, both wait modes) is the ping's own loop accounting - iterations_per_rtt, and in poll mode
    # spin_some_us and sleep_us - kept in the row after RESULT's fields, whose order it does not change.
    # The ping's own log (stderr) is kept per row with the pong's in $OUT.logs, for questions such as which peers
    # rmw_tickle's publisher registered.
    # PINGMAPS (2026-09-27): the rmw library the ping actually loaded, read from its /proc/PID/maps 2 s into the run by
    # a reader pinned to core 0 (the ping runs on 1-3). Its PID is found by parentage from the launch's own $! (timeout
    # -> time -> ping_node, taskset having exec'd) and verified by /proc/PID/exe - never by name.
    # Under perf stat (INSN_COUNT=1) the chain is timeout -> time -> perf (taskset exec'd) -> ping_node: one level deeper.
    if [ -n "$CNTSTEM" ]; then
        # Counter rows (COUNT_RMWS, header): the receiving thread chosen by exact task name, its counters started now.
        sh_ "$SERVER" "rm -f /tmp/c12_cnt.*; cd /proc/$pongpid/task || exit 0; for t in *; do echo \"task \$t \$(cat \$t/comm)\"; done > /tmp/c12_cnt.tasks
t=$pongpid; case '$rmw' in rmw_cyclonedds*) t=\$(awk '\$3 == \"recvUC\" {print \$2; exit}' /tmp/c12_cnt.tasks); [ -n \"\$t\" ] || t=\$(awk '\$3 == \"recv\" {print \$2; exit}' /tmp/c12_cnt.tasks);; esac
echo \"counted \$t\" >> /tmp/c12_cnt.tasks
setsid nohup perf stat -x, -e dTLB-load-misses,L1-dcache-load-misses,instructions,cycles -t \$t -o /tmp/c12_cnt.perf -- sleep $COUNT_S > /dev/null 2>&1 < /dev/null &
setsid nohup timeout $((COUNT_S + 1)) sudo -n bpftrace /tmp/c12_migr.bt \$t > /tmp/c12_cnt.migr 2>&1 < /dev/null &" || true
    fi
    local profpid=""
    if [ "$PROF" = 1 ]; then
        profpid=$(sh_ "$SERVER" "rm -f /tmp/c12_prof.*; cd /proc/$pongpid/task || exit 0; for t in *; do echo \"task \$t \$(cat \$t/comm)\"; done > /tmp/c12_prof.tasks
t=$pongpid; case '$rmw' in rmw_cyclonedds*) t=\$(awk '\$3 == \"recvUC\" {print \$2; exit}' /tmp/c12_prof.tasks); [ -n \"\$t\" ] || t=\$(awk '\$3 == \"recv\" {print \$2; exit}' /tmp/c12_prof.tasks);; esac
echo \"profiled \$t\" >> /tmp/c12_prof.tasks
setsid nohup timeout $PROF_S sudo -n bpftrace /tmp/c12_profile.bt \$t > /tmp/c12_prof.txt 2>&1 < /dev/null & echo \$!" || true)
        sh_ "$SERVER" "for i in \$(seq 1 20); do grep -q '^# c12 prof' /tmp/c12_prof.txt 2>/dev/null && break; sleep 0.25; done" || true
    fi
    if [ -n "$WARMSTEM" ] && [ "$WARM" != off ]; then
        local wcpu=$WARM_CPU_SAME
        [ "$WARM" = other ] && wcpu=$WARM_CPU_OTHER
        sh_ "$SERVER" "rm -f /tmp/c12_warm.log; setsid nohup taskset -c $wcpu /tmp/c12_warmer $WARM_S > /tmp/c12_warm.log 2>&1 < /dev/null &" || true
    fi
    if [ -n "$INJSTEM" ]; then
        # The injector's client and server addresses on the test link, read from the hosts rather than assumed.
        local cip sip
        cip=$(sh_ "$CLIENT" "ip -4 -o addr show eth0 | awk '{print \$4}' | cut -d/ -f1 | head -1")
        sip=$(sh_ "$SERVER" "ip -4 -o addr show eth0 | awk '{print \$4}' | cut -d/ -f1 | head -1")
        sh_ "$CLIENT" "rm -f /tmp/c12_inj.log; setsid nohup taskset -c 0 python3 /tmp/c12_idle_wake_injector.py $CAP_FILE $cip $sip $INJ 45 3 > /tmp/c12_inj.log 2>&1 < /dev/null &"
    fi
    res=$(sh_ "$CLIENT" "$env; $sstenv $stamprm $pingrm timeout 60 /usr/bin/time -f 'ping_utime_s=%U ping_stime_s=%S ping_maxrss_kb=%M' -o /tmp/rmwx_ping_time.txt taskset -c 1-3 $pingpre $BIN/ping_node -i 0.1 -d 10 $flag $waitflag $stampflag -m $msg 2>/tmp/rmwx_ping.log >/tmp/rmwx_ping.out & tp=\$!; pp=; for i in \$(seq 1 50); do for c in \$(cat /proc/\$tp/task/\$tp/children 2>/dev/null); do for g in \$c \$(cat /proc/\$c/task/\$c/children 2>/dev/null); do for q in \$g \$(cat /proc/\$g/task/\$g/children 2>/dev/null); do [ \"\$(readlink /proc/\$q/exe 2>/dev/null)\" = $BIN/ping_node ] && pp=\$q; done; done; done; [ -n \"\$pp\" ] && break; sleep 0.1; done; sleep 2; echo PINGMAPS: \$(taskset -c 0 grep -o '/[^ ]*librmw_[a-z_]*\\.so' /proc/\$pp/maps 2>/dev/null | sort -u | tr '\\n' ' ') >/tmp/rmwx_ping_maps.txt; wait \$tp; cat /tmp/rmwx_ping.out /tmp/rmwx_ping_time.txt /tmp/rmwx_ping_maps.txt" | grep -E '^RESULT:|^LOOP:|^PHASE:|^WARMUP:|^ping_utime_s|^PINGMAPS:' | tr '\n' ' ' || true)
    res="$res $procs${idle:+ $idle}"
    if [ "$PROF" = 1 ]; then
        mkdir -p "$OUT.prof"
        sh_ "$SERVER" "for i in \$(seq 1 60); do [ -d /proc/${profpid:-0} ] || break; sleep 0.5; done; cat /tmp/c12_prof.tasks; cat /tmp/c12_prof.txt" >"$OUT.prof/$stem.txt" 2>&1 || true
        res="$res prof_samples=$(grep -c '^S ' "$OUT.prof/$stem.txt" 2>/dev/null)"
    fi
    if [ -n "$WARMSTEM" ]; then
        mkdir -p "$OUT.pin"
        sh_ "$SERVER" "grep Cpus_allowed_list /proc/$pongpid/status; for t in /proc/$pongpid/task/*; do echo \"task \${t##*/} \$(cut -d')' -f2 \$t/stat | awk '{print \$37}') \$(cat \$t/comm)\"; done" >"$OUT.pin/$stem.txt" 2>&1 || true
        res="$res warm=$WARM pin=$(grep -o 'Cpus_allowed_list:.*' "$OUT.pin/$stem.txt" | awk '{print $2}')"
        if [ "$WARM" != off ]; then
            sh_ "$SERVER" "for i in \$(seq 1 40); do grep -q '^WARMER_END' /tmp/c12_warm.log && break; sleep 0.25; done; cat /tmp/c12_warm.log" >"$OUT.pin/$stem.warmer.txt" 2>&1 || true
            res="$res $(grep -m1 '^WARMER_END' "$OUT.pin/$stem.warmer.txt" || echo warmer_end=missing)"
        fi
    fi
    if [ -n "$CNTSTEM" ]; then
        mkdir -p "$OUT.counters"
        sh_ "$SERVER" "for i in \$(seq 1 40); do grep -q '^@\|^Attaching\|ERROR' /tmp/c12_cnt.migr 2>/dev/null && [ -s /tmp/c12_cnt.perf ] && grep -q cycles /tmp/c12_cnt.perf && grep -q '^@' /tmp/c12_cnt.migr && break; sleep 0.5; done; cat /tmp/c12_cnt.tasks; echo '--- perf'; cat /tmp/c12_cnt.perf; echo '--- migr'; cat /tmp/c12_cnt.migr" >"$OUT.counters/$stem.txt" 2>&1 || true
        res="$res counters=$(grep -c ',cycles,' "$OUT.counters/$stem.txt" 2>/dev/null)"
    fi
    local injvoid=""
    if [ -n "$INJSTEM" ]; then
        # The injector ends by itself 3 s after the last ping (45 s at most); its INJECT_END line is always printed.
        mkdir -p "$OUT.inject"
        sh_ "$CLIENT" "for i in \$(seq 1 60); do grep -q '^INJECT_END' /tmp/c12_inj.log && break; sleep 0.25; done; cat /tmp/c12_inj.log" >"$OUT.inject/$stem.txt" 2>&1 || true
        local ie isent
        ie=$(grep -m1 '^INJECT_END' "$OUT.inject/$stem.txt" | cut -d' ' -f2-)
        isent=$(echo "$ie" | grep -oE 'sent=[0-9]+' | cut -d= -f2)
        res="$res inject=$INJ ${ie:-inject_end=missing}"
        if [ -z "$ie" ]; then
            injvoid="VOID(inject: no INJECT_END)"
        elif [ "$INJ" = none ] && [ "${isent:-1}" != 0 ]; then
            injvoid="VOID(inject: none arm sent $isent)"
        elif [ "$INJ" != none ] && [ "${isent:-0}" -lt 50 ]; then
            injvoid="VOID(inject: lead $INJ sent only ${isent:-0})"
        fi
    fi
    if [ "$BPF" = on ]; then
        sh_ "$SERVER" "[ \"\$(readlink /proc/$bpfpid/exe)\" = /usr/bin/timeout ] && kill -TERM $bpfpid; for i in \$(seq 1 20); do [ -d /proc/$bpfpid ] || break; sleep 0.5; done" || true
        mkdir -p "$OUT.bpf"
        scp -q -i "$K" -o BatchMode=yes "ci@$SERVER:/tmp/rmwx_bpf.txt" "$OUT.bpf/$stem.txt" 2>/dev/null || say "  (no bpftrace output)"
        grep -q '^# pong_rx_split' "$OUT.bpf/$stem.txt" 2>/dev/null || { [ "$verdict" = ok ] && verdict="VOID(bpftrace did not attach)"; }
    fi
    local pongcpu
    # pong_cpu_ns: summed run time of every pong thread from /proc/PID/task/*/schedstat (ns), because the
    # tick-based utime+stime (pong_cpu_s, 10 ms granularity) cannot separate rmw_tickle from CycloneDDS.
    pongcpu=$(sh_ "$SERVER" "[ -d /proc/$pongpid ] && awk -v t=\$(getconf CLK_TCK) '{printf \"pong_cpu_s=%.2f \", (\$14+\$15)/t}' /proc/$pongpid/stat && cat /proc/$pongpid/task/*/schedstat | awk '{s+=\$1} END{printf \"pong_cpu_ns=%d \", s}' && awk '/VmHWM/{printf \"pong_maxrss_kb=%s\", \$2}' /proc/$pongpid/status" 2>/dev/null || true)
    res="$res $pongcpu"
    sh_ "$SERVER" "[ -d /proc/$pongpid ] && [ \"\$(readlink /proc/$pongpid/exe)\" = $BIN/pong_node ] && kill -INT $pongpid" >/dev/null 2>&1 || true
    sleep 1
    if [ "$INSN_COUNT" = 1 ]; then
        # perf writes its counts when its child has gone: wait up to 5 s for perf itself, then read both processes'.
        local pongc pingc pingf pongf
        pongc=$(sh_ "$SERVER" "for i in \$(seq 1 50); do [ -d /proc/$pongperf ] || break; sleep 0.1; done; cat /tmp/rmwx_pong.insn 2>/dev/null; rm -f /tmp/rmwx_pong.insn" || true)
        pingc=$(sh_ "$CLIENT" "cat /tmp/rmwx_ping.insn 2>/dev/null; rm -f /tmp/rmwx_ping.insn" || true)
        pingf=$(insn_fields "$pingc" | sed 's/insn/ping_insn/g')
        pongf=$(insn_fields "$pongc" | sed 's/insn/pong_insn/g')
        res="$res $pingf $pongf"
        case "$pingf $pongf" in *"ping_insn=ok "*"pong_insn=ok") ;; *) [ -n "$insn_void" ] || insn_void="VOID(insn: $pingf $pongf)" ;; esac
    fi
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
    # 2026-10-18: which side lost a round trip. rmw_tickle's pong counts each reply it published with no matched reader
    # (tx_unmatched_dropped, BEST_EFFORT; tx_unmatched_kept, RELIABLE, owed to no VOLATILE reader). Until the ping's
    # warm-up (WARMUP:) that was the reply to seq 1 whenever the pong heard the first ping before the ping's
    # subscriber: 10 of 112 J reps at 18f16b85, all VOID(loss) with exactly 1 here, every other rep 0
    # (experiments/rmw_loss_attribution.py). Printed only when the pong log holds the counters, never as a guessed 0.
    case "$rmw" in rmw_tickle*)
        local unm
        unm=$(grep -oE 'tx_unmatched_dropped=[0-9]+ tx_unmatched_kept=[0-9]+' "$OUT.logs/${stem}_pong.log" 2>/dev/null \
            | head -1 | awk -F'[= ]' '{print $2 + $4}')
        [ -n "$unm" ] && res="$res pong_unmatched=$unm" ;;
    esac
    if [ "$CAPTURE" = 1 ]; then
        local off1; off1=$(clock_offset)
        say "  clock_offset_ns stem=$stem before: ${off0}after: ${off1}"
        cap_collect "$stem"
    fi
    case "$rmw" in rmw_tickle*)
        sleep 1; mkdir -p "$OUT.dumps"
        scp -q -i "$K" -o BatchMode=yes "ci@$SERVER:/tmp/rmwx_dump.txt" "$OUT.dumps/${rmw}_${msg}_${qos}_rep${rep}${INJSTEM}.txt" 2>/dev/null \
            || say "  (no dump from $rmw rep$rep)" ;;
    esac
    case "$maps" in *"$(lib_for "$rmw")"*) ;; *) verdict="VOID(pong loaded: ${maps:-nothing})" ;; esac
    case "$res" in *"PINGMAPS: "*"$(lib_for "$rmw")"*) ;;
        *) [ "$verdict" = ok ] && verdict="VOID(ping did not show $(lib_for "$rmw") in its maps)" ;; esac
    # ... and no other implementation beside it, on either side
    local impls
    impls=$(echo "${res#*PINGMAPS: } | $maps" | tr ' ' '\n' | grep -oE 'librmw_(tickle|fastrtps_cpp|cyclonedds_cpp|zenoh_cpp)\.so' | sort -u | wc -l)
    [ "$impls" = 1 ] || { [ "$verdict" = ok ] && verdict="VOID($impls rmw implementations loaded across ping and pong)"; }
    case "$res" in *"framework=${rmw%@*} "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(no RESULT for $rmw)" ;; esac
    case "$res" in *"loss_pct=0 "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(loss)" ;; esac
    # Every ping_node since 2026-10-18 warms up before seq 1; a row without the line ran a binary that does not.
    case "$res" in *"WARMUP: warmup=on "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(no WARMUP: warmup=on; ping_node without the warm-up)" ;; esac
    if [ -n "$IDLE_S" ] && [ "$TRACE" != 1 ]; then
        case "$res" in *"pong_idle_cpu_ns="*) ;; *) [ "$verdict" = ok ] && verdict="VOID(no idle CPU reading)" ;; esac
    fi
    if [ "$WAITS" != poll ]; then
        case "$res" in *" wait=$WAIT "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(ping did not run wait=$WAIT)" ;; esac
    fi
    if [ -n "$PSLEEP" ]; then
        case "$res" in *"LOOP: poll_sleep_us=$PSLEEP "*) ;; *) [ "$verdict" = ok ] && verdict="VOID(ping did not sleep $PSLEEP us)" ;; esac
    fi
    if [ "$WAIT" = poll ] && { [ -n "$PHASE" ] || [ -n "$RTT_AT" ]; }; then
        case "$res" in *"PHASE: phase=${PHASE:-locked} rtt_at=${RTT_AT:-loop} "*) ;;
            *) [ "$verdict" = ok ] && verdict="VOID(ping did not run phase=${PHASE:-locked} rtt_at=${RTT_AT:-loop})" ;; esac
        if [ "$PHASE" = jitter ]; then
            local chi2
            chi2=$(echo "$res" | grep -oE 'PHASE: [^|]* chi2=[0-9.]+' | grep -oE '[0-9.]+$')
            awk -v c="${chi2:-999}" 'BEGIN { exit !(c < 21.67) }' \
                || { [ "$verdict" = ok ] && verdict="VOID(jitter phase not uniform, chi2=${chi2:-none})"; }
        fi
    fi
    case "${ARM_TAG:-}" in *VOID-freq*) [ "$verdict" = ok ] && verdict="VOID(spinner did not lift the clock)" ;; esac
    case "$EP:$rmw" in
    on:rmw_tickle* | off:rmw_tickle*)
        grep -q "rmw_tickle: executor_poll=$([ "$EP" = on ] && echo 1 || echo 0) " "$OUT.logs/${stem}_ping.log" 2>/dev/null \
            || { [ "$verdict" = ok ] && verdict="VOID(executor_poll=$EP not confirmed by the ping)"; } ;;
    esac
    # Gate v2 A2: a counted row without both counts is VOID - "could not look" is never "no work".
    if [ -n "$insn_void" ] && [ "$verdict" = ok ]; then verdict=$insn_void; fi
    if [ -n "$injvoid" ] && [ "$verdict" = ok ]; then verdict=$injvoid; fi
    say "$rmw $msg $qos rep$rep${WAITSTEM:+ wait=$WAIT}${PSLEEP:+ poll_sleep_us=$PSLEEP}${SSTSTEM:+ sst=on}${BPFSTEM:+ bpf=on}${EPSTEM:+ execpoll=$EP}${PONG_THREADS:+ pong_threads=$PONG_THREADS}${INJSTEM:+ inject=$INJ}${CNTSTEM:+ counted=1}${WARMSTEM:+ warm=$WARM}${ARM_TAG:-} | $verdict | ${res#RESULT: }"
    if [ "$TRACE" = 1 ]; then
        sleep 1; mkdir -p "$OUT.traces"
        scp -q -i "$K" -o BatchMode=yes "ci@$SERVER:/tmp/rmwx_trace_$rmw.txt" "$OUT.traces/$rmw.txt" || say "  (trace copy failed for $rmw)"
    fi
}
# The router goes up once, before any measurement, and comes down in the exit handler. Failing here is fatal rather
# than warned: without it every zenoh row voids, which is a correct refusal but a wasted run.
zenohd_on || exit 1

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
        qoses=$QOSES; [ "$TRACE" = 1 ] && qoses=best_effort
        for qos in $qoses; do
            RMWS="$RMW_LIST"
            if [ -n "$TICKLE_VARIANTS" ]; then
                RMWS=""; for v in $TICKLE_VARIANTS; do RMWS="$RMWS rmw_tickle@$v"; done
                RMWS="$RMWS $VARIANT_VENDORS"
            fi
            # ROTATE=1 (2026-10-21): the rmw order rotates by one each repetition, so no rmw always runs first after a
            # build or always follows the same one. Unset, the order is RMW_LIST's in every repetition, as before.
            if [ "${ROTATE:-0}" = 1 ]; then
                read -r -a rot <<<"$RMWS"
                RMWS=""; for i in $(seq 0 $((${#rot[@]} - 1))); do RMWS="$RMWS ${rot[$(((i + rep - 1) % ${#rot[@]}))]}"; done
            fi
            for WAIT in $WAITS; do
              WAITSTEM=""; [ "$WAITS" != poll ] && WAITSTEM="_$WAIT"
              psleeps="-"; [ "$WAIT" = poll ] && [ -n "$POLL_SLEEPS" ] && psleeps="$POLL_SLEEPS"
              for PS in $psleeps; do
                PSLEEP=""; [ "$PS" != - ] && PSLEEP="$PS"
                for SST in $SYSSTAMP_ARMS; do
                  SSTSTEM=""; [ "$SST" = on ] && SSTSTEM="_sst"
                  for BPF in $BPF_ARMS; do
                    BPFSTEM=""; [ "$BPF" = on ] && BPFSTEM="_bpf"
                    for EP in $EXEC_POLL_ARMS; do
                      EPSTEM=""; [ "$EP" != - ] && EPSTEM="_ep$EP"
                      INJS="$INJECT_ARMS"
                      if [ "${ROTATE:-0}" = 1 ]; then
                          read -r -a irot <<<"$INJECT_ARMS"
                          INJS=""; for i in $(seq 0 $((${#irot[@]} - 1))); do INJS="$INJS ${irot[$(((i + rep - 1) % ${#irot[@]}))]}"; done
                      fi
                      for INJ in $INJS; do
                        INJSTEM=""; [ "$INJ" != - ] && INJSTEM="_inj$INJ"
                        WARMS="$WARM_ARMS"
                        if [ "$WARM_ARMS" != - ] && [ "${ROTATE:-0}" = 1 ]; then
                            read -r -a wrot <<<"$WARM_ARMS"
                            WARMS=""; for i in $(seq 0 $((${#wrot[@]} - 1))); do WARMS="$WARMS ${wrot[$(((i + rep - 1) % ${#wrot[@]}))]}"; done
                        fi
                        for WARM in $WARMS; do
                          WARMSTEM=""; [ "$WARM" != - ] && WARMSTEM="_warm$WARM"
                          for rmw in $RMWS; do
                            case "$EP:$rmw" in on:rmw_tickle*) ;; on:*) continue ;; esac
                            if [ "$WARM" != - ] && [ "$WARM" != off ]; then
                              case " $WARM_RMWS " in *" $rmw "*) ;; *) continue ;; esac
                            fi
                            one "$rmw" "$msg" "$qos" "$rep"
                          done
                        done
                        WARMSTEM=""; WARM=off
                      done
                      if [ "$rep" -le "$COUNT_REPS" ]; then
                        CNTSTEM="_cnt"
                        for INJ in $INJS; do
                          INJSTEM=""; [ "$INJ" != - ] && INJSTEM="_inj$INJ"
                          for rmw in $RMWS; do
                            case " $COUNT_RMWS " in *" $rmw "*) one "$rmw" "$msg" "$qos" "$rep" ;; esac
                          done
                        done
                        CNTSTEM=""
                      fi
                    done
                  done
                done
              done
            done
        done
    done
  done
done
say ""; say "=== done ==="
