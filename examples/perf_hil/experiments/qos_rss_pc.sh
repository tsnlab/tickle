#!/usr/bin/env bash
# Does the QoS creation API raise the benches' peak RSS? main against the QoS branch, on THIS PC, in a private network
# namespace. A PC figure for the decision to take it to the rig, never a published one.
#
# WHAT CHANGED. QoS step 2b moved the benches' storage from static arrays (.bss) to calloc() of tt_*_storage_size()
# bytes, and core's creation path memset the reorder slots, tracking windows and cache index - every page resident at
# creation: ~0.4 MB at p1, ~6 MB at p4 for a RELIABLE Subscriber's 4,096 reorder slots. Fix A (qos_zero_dirty_pages(),
# src/tickle.c) clears only pages that are not zero already, which a fresh calloc's are not.
#
# ARMS: main (MAIN, before the creation API), pre (PRE, the branch before fix A), fix (FIX, the branch with it).
# CELLS: reliable_throughput and reliable_latency at p1 and p4, client and server; best_effort_throughput p1 as the
#   CONTROL: a BEST_EFFORT Publisher retains nothing and a BEST_EFFORT Subscriber holds no reorder slots, so neither
#   ever calls the clearing that pre and fix differ in. Interleaved per rep, the arm order rotating.
#
# HOW TO READ IT, written before running and enforced in the reading at the end (qos_rss_read.py).
#   MEASURE: each process's own VmHWM - peak_rss_kb on its RESULT line, or for the latency server, which prints none,
#     its VmHWM read from /proc while it runs (hwm_follow, HWM lines). ru_maxrss (GNU time) is printed beside them. INSTRUMENT: where a bench also prints its own
#     VmHWM (peak_rss_kb), the two must agree within 5%, or time's floor binds and the whole reading is VOID.
#   SAMPLES: a process counts only if it exited 0, and a throughput server only if its RESULT says lost=0 (a lossy run
#     touches reorder slots in proportion to what it lost, which is not what is compared); fewer than 3 such reps per
#     arm is VOID for that row.
#   TREATMENT: pre's reliable_latency client and server (each has a Subscriber with 4,096 reorder slots;
#     reliable_throughput's server has only its window's) must sit at least 300 KB above main's at p1 and 4,000 KB at
#     p4, the step 2b finding. If it does not, this instrument cannot see the effect fix A is for, and its "no increase"
#     says nothing: the whole reading is VOID.
#   CONTROL: |median(fix) - median(pre)| of the control cell is this PC's run-to-run noise for that pair; the noise
#     floor used below is max(64 KB, 2 x that).
#   READING per cell, delta = median(fix) - median(main):
#     SAME   |delta| <= floor
#     LOWER  delta < -floor
#     HIGHER delta > floor - a rise fix A must explain byte by byte, or it fails.
#
# Usage: [MAIN=origin/main] [PRE=<rev>] [FIX=HEAD] [REPS=3] [DUR=6] [CELLS="<scenario>:<pN> ..."] qos_rss_pc.sh
#   (all revisions committed; CELLS must keep reliable_latency p1 and p4 and the control, or the reading is VOID)
# Launch detached; results in ~/rig_results_safe/. Never touches the rig. Runs niced: the PC is shared.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=box
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
MAIN=${MAIN:-origin/main}
PRE=${PRE:?set PRE to the branch commit before fix A}
FIX=${FIX:-HEAD}
REPS=${REPS:-3}
DUR=${DUR:-6}
declare -A REV
REV[main]=$(git -C "$REPO" rev-parse --short "$MAIN") || exit 1
REV[pre]=$(git -C "$REPO" rev-parse --short "$PRE") || exit 1
REV[fix]=$(git -C "$REPO" rev-parse --short "$FIX") || exit 1
NS=tickle_qosrss_$$
OUT=${OUT:-$HOME/rig_results_safe/qos_rss_pc_${REV[main]}_${REV[pre]}_${REV[fix]}_$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"
WORK=$(mktemp -d /home/semih/tmp_build/qosrss.XXXXXX)
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

CELLS=${CELLS:-"reliable_throughput:p1 reliable_throughput:p4 reliable_latency:p1 reliable_latency:p4 best_effort_throughput:p1"}
say "=== QoS peak RSS, PC, main=${REV[main]} pre=${REV[pre]} fix=${REV[fix]} reps=$REPS dur=$DUR $(date -Is) ==="
for arm in main pre fix; do
    mkdir -p "$WORK/src_$arm" "$WORK/home_$arm"
    git -C "$REPO" archive "${REV[$arm]}" | tar -x -C "$WORK/src_$arm" || { say "FATAL cannot export ${REV[$arm]}"; exit 1; }
    for cell in $CELLS; do
        sc=${cell%:*} sz=${cell#*:}
        # Each arm installs libtickle.a under its own HOME: build.sh reinstalls only when src/ is newer than the
        # installed library, so a shared prefix would hand one arm the other's core.
        (cd "$WORK/src_$arm/examples/perf_hil/tickle" && HOME="$WORK/home_$arm" TMPDIR=/home/semih/tmp_build \
            nice -n 19 ./build.sh "$sc" "$sz") >"$WORK/build_${arm}_${sc}_$sz.log" 2>&1 ||
            { say "FATAL build $arm $sc $sz"; tail -5 "$WORK/build_${arm}_${sc}_$sz.log" | tee -a "$OUT"; exit 1; }
        mkdir -p "$WORK/bin_${arm}_${sc}_$sz"
        cp "$WORK/src_$arm/examples/perf_hil/tickle/${sc}_$sz/client" \
            "$WORK/src_$arm/examples/perf_hil/tickle/${sc}_$sz/server" "$WORK/bin_${arm}_${sc}_$sz/"
        say "IDENTITY arm=$arm cell=${sc}_$sz client=$(sha256sum "$WORK/bin_${arm}_${sc}_$sz/client" | cut -c1-16)" \
            "server=$(sha256sum "$WORK/bin_${arm}_${sc}_$sz/server" | cut -c1-16)"
    done
done

sudo -n ip netns add "$NS" || { say "FATAL cannot create netns"; exit 1; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo
sudo -n ip netns exec "$NS" ip link add bench0 type dummy
sudo -n ip netns exec "$NS" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip netns exec "$NS" ip link set bench0 up

# GNU time's %M is wait4()'s ru_maxrss, which counts the parent's resident set at exec as a floor: under python
# (rusage_run.py) that floor was ~12 MB, above every bench. time's own is ~1.6 MB, below them; the reader checks that it
# does not bind, against the VmHWM the benches that print peak_rss_kb read themselves.
TIMEFMT='RUSAGE exit=%x maxrss_kb=%M'
# The server's own VmHWM, read from outside while it runs (the latency server prints none, and ru_maxrss read 5-12%
# low): the bench is found by ancestry from the PID this script launched - each process's first child, down to the one
# whose /proc/<pid>/comm is "server" - never by a command-line pattern. The last reading before it exits is kept;
# VmHWM never falls, and what a run allocates after its last reading (teardown) is not its peak.
hwm_follow() { # hwm_follow <launched pid> <out file>
    local root=$1 out=$2 pid kids hwm
    : >"$out"
    while [ -d "/proc/$root" ]; do
        pid=$root
        while [ -n "$pid" ] && [ "$(cat "/proc/$pid/comm" 2>/dev/null)" != server ]; do
            kids=$(cat "/proc/$pid/task/$pid/children" 2>/dev/null)
            pid=${kids%% *}
        done
        if [ -n "$pid" ]; then
            hwm=$(awk '/^VmHWM:/ {print $2}' "/proc/$pid/status" 2>/dev/null)
            [ -n "$hwm" ] && echo "HWM pid=$pid vmhwm_kb=$hwm" >"$out"
        fi
        sleep 0.2
    done
}
run_cell() { # run_cell <rep> <scenario> <size> <arm>
    local rep=$1 sc=$2 sz=$3 arm=$4 cargs
    local bin="$WORK/bin_${arm}_${sc}_$sz" srv_d=$((DUR + 8))
    # The rig cells' QoS (campaign_sweep.sh common_args), at a rate this shared PC carries without loss: what is
    # compared is what creation and a lossless run make resident, where a lossy run would touch reorder slots in
    # proportion to what it happened to lose.
    case "$sc:$sz" in
    reliable_throughput:p1) cargs="-Q -N 2048 -B 100 -i 0.0002" ;;
    reliable_throughput:p4) cargs="-Q -N 187 -B 100 -i 0.0002" ;;
    reliable_latency:*) cargs="-i 0.005" ;;
    *) cargs="-i 0.0002" ;;
    esac
    # The latency server's -d is the client's measured window, and it adds 15 s and the warm-up and cool-down bounds
    # before it stops by itself (on the rig run_scenario.sh SIGINTs it; here nothing may signal a root process). So it
    # gets the client's -d, and the time to reach its own end.
    [ "$sc" = reliable_latency ] && srv_d=$DUR
    # shellcheck disable=SC2024 # the logs stay ours
    sudo -n ip netns exec "$NS" env BENCH_IFACE=lo timeout $((srv_d + 90)) nice -n 19 /usr/bin/time -f "$TIMEFMT" \
        "$bin/server" -d "$srv_d" >"$WORK/srv.log" 2>&1 &
    local srv=$!
    hwm_follow "$srv" "$WORK/srv.hwm" &
    local follower=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086 # cargs is meant to split
    sudo -n ip netns exec "$NS" env BENCH_IFACE=lo timeout $((srv_d + 30)) nice -n 19 /usr/bin/time -f "$TIMEFMT" \
        "$bin/client" -d "$DUR" $cargs >"$WORK/cli.log" 2>&1
    wait "$srv"
    wait "$follower"
    local tag="rep=$rep cell=${sc}_$sz arm=$arm"
    { grep -h '^RESULT' "$WORK/cli.log" | sed "s/^/$tag role=client /"
        grep -h '^RUSAGE' "$WORK/cli.log" | sed "s/^/$tag role=client /"
        grep -h '^RESULT' "$WORK/srv.log" | sed "s/^/$tag role=server /"
        grep -h '^RUSAGE' "$WORK/srv.log" | sed "s/^/$tag role=server /"
        sed "s/^/$tag role=server /" "$WORK/srv.hwm"; } >>"$OUT"
    grep -q '^RUSAGE.* exit=0 ' "$WORK/cli.log" || { say "$tag: client did not exit 0; its last lines:"; tail -4 "$WORK/cli.log" | tee -a "$OUT"; }
    grep -q '^RUSAGE.* exit=0 ' "$WORK/srv.log" || { say "$tag: server did not exit 0; its last lines:"; tail -4 "$WORK/srv.log" | tee -a "$OUT"; }
}

for rep in $(seq 1 "$REPS"); do
    case $((rep % 3)) in
    1) order="main pre fix" ;;
    2) order="pre fix main" ;;
    *) order="fix main pre" ;;
    esac
    say "--- rep $rep/$REPS $(date -Is) order: $order load: $(cut -d' ' -f1-3 /proc/loadavg) ---"
    for cell in $CELLS; do
        for arm in $order; do
            run_cell "$rep" "${cell%:*}" "${cell#*:}" "$arm"
        done
    done
done
say "=== runs done $(date -Is) ==="
python3 "$REPO/examples/perf_hil/experiments/qos_rss_read.py" "$OUT" | tee -a "$OUT"
say "QOS_RSS_DONE"
