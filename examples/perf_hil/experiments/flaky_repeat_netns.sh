#!/usr/bin/env bash
# flaky_repeat_netns.sh OUT_DIR N MODE SETUP -- COMMAND...
#
# Runs COMMAND N times in a private netns, to reproduce an intermittent failure without generating load on this shared
# PC. The timing pressure comes from constraining the process itself, not from anything else running:
#   MODE free   no constraint (the control: what the same build does unconstrained)
#   MODE core1  every thread of COMMAND pinned to one CPU (taskset; CPU=n picks it, default 11), so its own threads
#               queue behind each other
#   MODE idle   core1 plus SCHED_IDLE: COMMAND also yields its CPU to whatever else this PC runs there. Harsher than a
#               gate run at load ~10 - a SCHED_IDLE thread can wait hundreds of ms - so a timing bound it misses is not
#               a finding by itself
# SETUP is a file sourced inside the netns before each run (ROS setup, LD_LIBRARY_PATH, ...). It is read again for each
# run, so it can be changed between runs; this script must not be.
#
# The netns has only lo, so every context in it is at 127.0.0.1, while /dev/shm - core's segments and context-id
# registry, named by address, port and context id - is shared with every other netns on this PC. Two loops at once, or
# a loop beside another session's lo-only test, keep apart only by port - and a ROS_DOMAIN_ID of their own does not
# keep them apart: a segment is named by the context's data port, which each netns allocates on its own, so two netns
# can hand out the same one. On 2026-10-29 concurrent test_loaned_messages loops met it ("Replacing segment ...
# although a live context owns it - two contexts share its name"), and those runs failed in ways a lone loop did not
# show in 600 runs (a RELIABLE sample 1127 delivered where 26 was due, a ring case that the ring carried under half of).
# Run one loop at a time, and grep a failing run's output for that warning before reading anything into it.
#
# Each run gets a 120 s timeout, 4 GB of address space (ulimit -v, unless NO_VLIMIT=1, which ASan needs), a core dump
# limit of unlimited (kernel.core_pattern decides where a core goes; on this PC apport puts it in
# /var/lib/apport/coredump), and runs in OUT_DIR/run_<i>, its output in out.log there. A passing run's directory is
# deleted to cap the disk used, unless KEEP_PASS=1; a failing one is kept.
#
# Reading, enforced below: each run's exit status is recorded in OUT_DIR/summary.txt, one line per run. The last line
# is always "DONE mode=... runs=N pass=P fail=F", success or not - something to wait on. A run that timed out is
# counted as fail and marked TIMEOUT, never as pass.
set -uo pipefail

OUT="$1"
N="$2"
MODE="$3"
SETUP="$4"
shift 4
[ "${1:-}" = "--" ] && shift
[ "$#" -gt 0 ] || {
    echo "usage: $0 OUT_DIR N MODE SETUP -- COMMAND..."
    exit 2
}
case "$MODE" in free | core1 | idle) ;; *)
    echo "MODE must be free, core1 or idle"
    exit 2
    ;;
esac
CPU="${CPU:-11}"
mkdir -p "$OUT" || exit 1
NS="flaky$$"
SUMMARY="$OUT/summary.txt"
: >"$SUMMARY"
cleanup() { sudo -n ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT # the only EXIT trap here

if ! { sudo -n ip netns add "$NS" && sudo -n ip netns exec "$NS" ip link set lo up &&
    sudo -n ip netns exec "$NS" ip route add default dev lo; }; then
    echo "DONE mode=$MODE runs=0 pass=0 fail=0 (no netns)" >>"$SUMMARY"
    exit 79
fi
# The netns must not be the default one: the run's view of its namespace inode differs from this shell's.
default_ns=$(readlink /proc/self/ns/net)
inside_ns=$(sudo -n ip netns exec "$NS" sudo -n -u "$USER" readlink /proc/self/ns/net)
if [ -z "$inside_ns" ] || [ "$inside_ns" = "$default_ns" ]; then
    echo "DONE mode=$MODE runs=0 pass=0 fail=0 (netns not proved: '$inside_ns')" >>"$SUMMARY"
    exit 79
fi
echo "netns $NS: $inside_ns (default $default_ns)" | tee -a "$OUT/header.txt"

prefix=""
case "$MODE" in
core1) prefix="taskset -c $CPU" ;;
idle) prefix="taskset -c $CPU chrt -i 0" ;;
esac
vlimit="ulimit -v 4194304;"
[ "${NO_VLIMIT:-0}" = 1 ] && vlimit=""
cmd=$(printf '%q ' "$@")

pass=0
fail=0
for i in $(seq 1 "$N"); do
    dir="$OUT/run_$i"
    mkdir -p "$dir"
    start=$(date +%s.%N)
    # shellcheck disable=SC2024  # the log is the invoking user's file, so the redirect deliberately stays outside sudo
    sudo -n ip netns exec "$NS" sudo -n -u "$USER" bash -c "
        set +u; source '$SETUP'; set -u
        cd '$dir' && $vlimit ulimit -c unlimited && exec timeout -s KILL 120 $prefix $cmd
    " >"$dir/out.log" 2>&1
    rc=$?
    took=$(echo "$(date +%s.%N) - $start" | bc)
    if [ "$rc" = 0 ]; then
        pass=$((pass + 1))
        echo "run $i rc=0 ${took}s" >>"$SUMMARY"
        [ "${KEEP_PASS:-0}" = 1 ] || rm -rf "$dir"
    else
        fail=$((fail + 1))
        tag=""
        [ "$rc" = 137 ] && tag=" TIMEOUT"
        echo "run $i rc=$rc${tag} ${took}s FAIL" >>"$SUMMARY"
    fi
done
echo "DONE mode=$MODE runs=$N pass=$pass fail=$fail" >>"$SUMMARY"
