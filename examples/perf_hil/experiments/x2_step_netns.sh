#!/usr/bin/env bash
# x2_step_netns.sh - X2 "step response" of the time-varying-network stage (release v2.2),
# ~/claude_reports/TIME_VARYING_EXPERIMENT_PLAN_2026-11-03.md section X2 with the pass rules of sections 7.2 and 8.2.
#
# README
#   Run (from the repository root, on the PC, when nothing else is measuring; it builds, creates network namespaces
#   and uses `sudo -n ip`; the node pinning needs CPUs 2, 4, 6):
#       setsid nohup bash examples/perf_hil/experiments/x2_step_netns.sh \
#           >"$HOME/rig_results_safe/time_varying/x2step_launch.log" 2>&1 </dev/null &
#   Defaults: HOLD=10 s, REPS=8 (delay D and loss LB), J_REPS=4 (jitter J), EST_TRACE=1 (estimator trace on),
#   NODE_PIN="2 4 6", D_BG_LOSS=2, CELLS="be_small rel_small rel_large". Output: ~/rig_results_safe/time_varying/
#   x2step_<stamp>/ (summary.txt ends with "=== x2_step_netns ended" and holds the reader's tables).
#   Expected duration: about 2.1 hours (180 runs: 4 reps x 27 + 4 reps x 18, each ~38-42 s at DUR=32, plus a 12 s
#   build; measured length of the T0 runs at DUR=30 was 37 s small / 40 s rel_large, not re-measured at DUR=32).
#   HOLD=2 or 0.5 are not supported by one invocation (the reader's V2 is VOID below a 2 s settle and the scored
#   window check assumes one DUR): run again with another HOLD and OUT. READ_ONLY=1 OUT=<dir> re-reads a finished OUT.
#
# DESIGN. One factor per arm; a tv run is DUR = 3 x HOLD + LEAD seconds: the first phase, ONE step (change) after
# LEAD + HOLD s, the second phase, the step back after another HOLD s, the third phase until the end. Each phase is
# thus held HOLD s (10 s: long enough for the estimators to settle, whose windows are a few seconds - the reader
# reports the settle time, it does not assume it). LEAD (2 s) covers discovery and the 1 s warm-up the nodes do not
# score. Exactly two changes are made (not a cycle): 1 -> 20 -> 1 ms (family D), 0 -> 5% -> 0 random loss (LB),
# 0 -> +-5 ms -> 0 jitter (J). D carries the constant D_BG_LOSS random loss in both phases (as time_varying_netns.sh:
# with nothing lost nothing is repaired, and the delay rules read repairs). Statics (s_d1 s_d20 s_l0 s_lb5 s_j0 s_j5)
# run the same DUR and the same reps; every phase condition of a tv arm is also a static arm, so a failure is
# attributed to the change or to the condition. Arms are named D_p<HOLD>, LB_p<HOLD>, J_p<HOLD>, s_<phase>, so the T0
# reader (time_varying_read.py, imported by x2_step_read.py) loads the runs unchanged.
#
# Copied from time_varying_netns.sh (same pattern, same file names per run): build, spec/shape/change_all (one
# `tc -batch` per namespace, three in parallel), topology, phases.txt stamped with CLOCK_MONOTONIC just before and
# after a change ("PHASE idx=<k> key=<phase> t_before_ns=<ns> t_after_ns=<ns> ok=<0|1>"), NODE_PIN, trace_err.
# Different: the changer makes the two steps and stops; ESTTRACE is on by default and goes to echo.err / ping.err.
# The one EXIT trap removes only the namespaces this run listed in its own file, and its work directory.
#
# Usage: [HOLD=10] [LEAD=2] [REPS=8] [J_REPS=4] [FAMILIES="D LB J"] [CELLS=...] [D_BG_LOSS=2] [PROFILE=ethernet|wifi]
#        [NODE_PIN="2 4 6"] [EST_TRACE=1] [OUT=dir] [READ_ONLY=1] x2_step_netns.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
HOLD=${HOLD:-10}
LEAD=${LEAD:-2}
REPS=${REPS:-8}
J_REPS=${J_REPS:-4}
FAMILIES=${FAMILIES:-"D LB J"}
CELLS=${CELLS:-"be_small rel_small rel_large"}
PROFILE=${PROFILE:-ethernet}
D_BG_LOSS=${D_BG_LOSS:-2}
EST_TRACE=${EST_TRACE:-1}
NODE_PIN=${NODE_PIN-"2 4 6"} # X0a: unpinned veth reorders on this PC; NODE_PIN="" runs unpinned on purpose
NODE_ENV=${NODE_ENV-TT_UDP_OFFLOAD=0} # one datagram per skb, as time_varying_netns.sh
SMALL_HZ=100
SMALL_BYTES=100
LARGE_BYTES=1048576
LARGE_HZ=${LARGE_HZ:-5}
[[ $HOLD =~ ^[0-9]+$ && $LEAD =~ ^[0-9]+$ && $REPS =~ ^[0-9]+$ && $J_REPS =~ ^[0-9]+$ ]] ||
    { echo "HOLD, LEAD, REPS, J_REPS are integers" >&2; exit 2; }
case "$EST_TRACE" in 0 | 1) ;; *) echo "EST_TRACE must be 0 or 1" >&2; exit 2 ;; esac
case "$PROFILE" in ethernet | wifi) ;; *) echo "PROFILE must be ethernet or wifi" >&2; exit 2 ;; esac
DUR=$((3 * HOLD + LEAD))
TRACE_DEF=()
[ "$EST_TRACE" = 1 ] && TRACE_DEF=(-Dtt_ESTIMATOR_TRACE)
LARGE_PROFILE=()
[ "$PROFILE" = wifi ] && LARGE_PROFILE=(-P wifi)
OUT=${OUT:-$HOME/rig_results_safe/time_varying/x2step_$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT/runs" || exit 1
SUM="$OUT/summary.txt"
say() { echo "$*" | tee -a "$SUM"; }

if [ "${READ_ONLY:-0}" = 1 ]; then
    python3 "$HERE/x2_step_read.py" "$OUT"
    exit $?
fi
: >"$SUM"

# family_phases FAMILY: its two phases, the first is where a run starts and ends.
family_phases() {
    case "$1" in
    D) echo "d1 d20" ;;
    J) echo "j0 j5" ;;
    LB) echo "l0 lb5" ;;
    *) return 1 ;;
    esac
}
STATICS="s_l0"
TV_ARMS=""
for fam in $FAMILIES; do
    phases=$(family_phases "$fam") || { echo "unknown family $fam (D, LB, J)" >&2; exit 2; }
    TV_ARMS="$TV_ARMS ${fam}_p$HOLD"
    for ph in $phases; do
        case " $STATICS " in *" s_$ph "*) ;; *) STATICS="$STATICS s_$ph" ;; esac
    done
done
ARMS="$STATICS$TV_ARMS"
# arm_reps ARM: how many reps the arm runs - J and its statics J_REPS, the rest REPS. s_l0 and the others are shared.
arm_reps() {
    case "$1" in
    s_j0 | s_j5 | J_p*) echo "$J_REPS" ;;
    *) echo "$REPS" ;;
    esac
}

WORK=$(mktemp -d "$HOME/tmp_build/x2step.XXXXXX") || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
TV_PID=""
on_exit() {
    local rc=$? ns
    [ -n "$TV_PID" ] && kill "$TV_PID" 2>/dev/null
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== x2_step_netns ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
TREE_SHA=$(git -C "$REPO" rev-parse --short HEAD)
DIRTY=$(git -C "$REPO" status --porcelain -- src include platform | wc -l)
say "=== x2_step_netns $(date -Is): tree=$TREE_SHA dirty_paths=$DIRTY PROFILE=$PROFILE REPS=$REPS J_REPS=$J_REPS" \
    "HOLD=$HOLD LEAD=$LEAD DUR=$DUR ARMS='$ARMS' CELLS='$CELLS' d_bg_loss=$D_BG_LOSS est_trace=$EST_TRACE ==="
{
    # the reader takes dur and d_bg_loss from here (time_varying_read.load)
    echo "tree=$TREE_SHA dirty_paths=$DIRTY profile=$PROFILE reps=$REPS dur=$DUR d_bg_loss=$D_BG_LOSS"
    echo "x2=1 hold=$HOLD lead=$LEAD j_reps=$J_REPS arms='$ARMS' cells='$CELLS' node_pin='${NODE_PIN:-}'" \
        "node_env='${NODE_ENV:-}'"
    echo "small_bytes=$SMALL_BYTES small_hz=$SMALL_HZ large_bytes=$LARGE_BYTES large_hz=$LARGE_HZ nproc=$(nproc)"
    echo "est_trace=$EST_TRACE"
    echo "build_small=cc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS ${EXTRA_CC:-}${TRACE_DEF:+ ${TRACE_DEF[*]}}"
    echo "build_large=cc -O2 -DNDEBUG -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0" \
        "-Dtt_RELIABLE_STATS -DR4_TRACE${TRACE_DEF:+ ${TRACE_DEF[*]}}"
} >>"$OUT/provenance.txt"

# --- build: both binaries from this tree, and the monotonic stamp (as time_varying_netns.sh) ---------------------------
CORE=("$REPO/src/tickle.c" "$REPO/src/encoding.c" "$REPO/src/log.c" "$REPO/src/hal_linux.c")
export TMPDIR=$HOME/tmp_build
# EXTRA_CC: extra compiler words for the small-sample node only (an A/B of one -D option), e.g. EXTRA_CC=-Dtt_RELIABLE_RETRY=1
# shellcheck disable=SC2086 # EXTRA_CC is a list of compiler words, split on purpose
if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS ${EXTRA_CC:-} "${TRACE_DEF[@]}" -I"$REPO/include" \
    -I"$REPO/src" \
    -I"$REPO/examples/perf_hil/tickle/common" -o "$WORK/node" "$HERE/wifi_link_node.c" "${CORE[@]}" -lpthread -lm \
    >"$WORK/build.log" 2>&1; then
    say "VOID  wifi_link_node did not build"
    tail -5 "$WORK/build.log"
    exit 1
fi
if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    -Dtt_RELIABLE_STATS -DR4_TRACE "${TRACE_DEF[@]}" -I"$REPO/include" -I"$REPO/src" \
    -I"$REPO/examples/perf_hil/tickle/common" \
    -o "$WORK/large" "$HERE/large_sample_bench.c" "${CORE[@]}" -lpthread -lm >>"$WORK/build.log" 2>&1; then
    say "VOID  large_sample_bench did not build"
    tail -5 "$WORK/build.log"
    exit 1
fi
cat >"$WORK/mono.c" <<'EOF'
#include <stdio.h>
#include <time.h>
int main(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("%llu\n", (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec);
    return 0;
}
EOF
if ! cc -O2 -o "$WORK/mono" "$WORK/mono.c" >>"$WORK/build.log" 2>&1; then
    say "VOID  the monotonic stamp did not build"
    exit 1
fi
MONO="$WORK/mono"

# --- shaping (netem words as time_varying_netns.sh spec(); only the phases X2 uses) ------------------------------------
spec() {
    local words
    case "$1" in
    d1) words="delay 1ms loss random ${D_BG_LOSS}%" ;;
    d20) words="delay 20ms loss random ${D_BG_LOSS}%" ;;
    j0) words="delay 6ms" ;;
    j5) words="delay 6ms 5ms" ;; # uniform +-5 ms: reorders, as jitter does; j0's 6 ms keeps the mean delay equal
    l0) words="delay 1ms" ;;
    lb5) words="delay 1ms loss random 5%" ;;
    *) return 1 ;;
    esac
    echo "limit 10000 $words ${NETEM_EXTRA:-}"
}
NODES=(p e i)
IPS=(10.78.0.1 10.78.0.2 10.78.0.3)
MACS=(02:78:00:00:00:01 02:78:00:00:00:02 02:78:00:00:00:03)
BCAST=10.78.0.255

topo() {
    local run=$1 k j ns
    HUB="x2$$-$run-h"
    echo "$HUB" >>"$NS_LIST"
    sudo -n ip netns add "$HUB" || return 1
    sudo -n ip -n "$HUB" link add br0 type bridge || return 1
    sudo -n ip -n "$HUB" link set br0 up
    for k in 0 1 2; do
        ns="x2$$-$run-${NODES[$k]}"
        echo "$ns" >>"$NS_LIST"
        NS[k]=$ns
        sudo -n ip netns add "$ns" || return 1
        sudo -n ip -n "$HUB" link add "hp$k" type veth peer name wl0 address "${MACS[$k]}" netns "$ns" || return 1
        sudo -n ip -n "$HUB" link set "hp$k" master br0 && sudo -n ip -n "$HUB" link set "hp$k" up || return 1
        sudo -n ip -n "$ns" addr add "${IPS[$k]}/24" brd + dev wl0 || return 1
        sudo -n ip -n "$ns" link set lo up && sudo -n ip -n "$ns" link set wl0 up || return 1
        shape_add "$ns" "$SHAPE_PHASE" || return 1
    done
    for k in 0 1 2; do
        for j in 0 1 2; do
            [ "$j" = "$k" ] && continue
            sudo -n ip -n "${NS[$k]}" neigh replace "${IPS[$j]}" lladdr "${MACS[$j]}" dev wl0 nud permanent || return 1
        done
    done
}
# shape_add NS PHASE: prio root, broadcast band 1:1 and unicast band 1:2, each a netem with the phase's words.
shape_add() {
    local ns=$1 words
    local t=(sudo -n ip netns exec "$ns" tc)
    words=$(spec "$2") || return 1
    "${t[@]}" qdisc add dev wl0 root handle 1: prio bands 2 priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 || return 1
    # shellcheck disable=SC2086 # the netem arguments are words on purpose
    "${t[@]}" qdisc add dev wl0 parent 1:1 handle 10: netem $words || return 1
    # shellcheck disable=SC2086
    "${t[@]}" qdisc add dev wl0 parent 1:2 handle 20: netem $words || return 1
    "${t[@]}" filter add dev wl0 parent 1: protocol ip prio 1 u32 match ip dst "$BCAST/32" flowid 1:1 || return 1
    "${t[@]}" filter add dev wl0 parent 1: protocol ip prio 1 u32 match ip dst 255.255.255.255/32 flowid 1:1 ||
        return 1
}
# change_all PHASE: every node's two netem qdiscs to PHASE at once - one `tc -batch` per namespace, in parallel.
change_all() {
    local words k batch="$RUN_DIR/phase_batch.tc" pids=() rc=0
    words=$(spec "$1") || return 1
    printf 'qdisc change dev wl0 parent 1:1 handle 10: netem %s\nqdisc change dev wl0 parent 1:2 handle 20: netem %s\n' \
        "$words" "$words" >"$batch" || return 1
    for k in 0 1 2; do
        sudo -n ip netns exec "${NS[$k]}" tc -batch "$batch" &
        pids+=($!)
    done
    for k in "${pids[@]}"; do wait "$k" || rc=1; done
    return $rc
}
# step_changer: LEAD s after its start, then HOLD s, step to the second phase; HOLD s later, back to the first; then
# it ends (two changes, no cycle). Each change is stamped before and after, as T0's tv_cycle. A child of this shell.
step_changer() {
    local idx=0 key before after ok
    sleep "$LEAD"
    for key in "${TV_PHASES[1]}" "${TV_PHASES[0]}"; do
        sleep "$HOLD"
        idx=$((idx + 1))
        before=$("$MONO")
        ok=1
        change_all "$key" || ok=0
        after=$("$MONO")
        echo "PHASE idx=$idx key=$key t_before_ns=$before t_after_ns=$after ok=$ok" >>"$RUN_DIR/phases.txt"
    done
}
counters() {
    local ns=$1 label=$2
    {
        echo "== $label tx_bytes=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/tx_bytes)" \
            "tx_packets=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/tx_packets)" \
            "tx_dropped=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/tx_dropped)" \
            "rx_dropped=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/rx_dropped)"
        sudo -n ip netns exec "$ns" tc -s qdisc show dev wl0
        echo "neigh $label: $(sudo -n ip -n "$ns" neigh show dev wl0 | tr '\n' ';')"
    } >>"$RUN_DIR/counters.txt"
}
inns() {
    local k=$1
    shift
    local pin=()
    if [ -n "${NODE_PIN:-}" ]; then # node k on the k-th CPU of NODE_PIN
        local cpus
        read -r -a cpus <<<"$NODE_PIN"
        pin=(taskset -c "${cpus[$k]}")
    fi
    # shellcheck disable=SC2086 # NODE_ENV is words on purpose
    sudo -n ip netns exec "${NS[$k]}" env ${NODE_ENV:-} bash -c 'ulimit -v 2000000; exec "$@"' inns "${pin[@]}" "$@"
}
# trace_err FILE CMD...: CMD with its stderr in FILE under EST_TRACE=1 (the ESTTRACE lines apart from the .log).
trace_err() {
    local file=$1
    shift
    if [ "$EST_TRACE" = 1 ]; then
        "$@" 2>"$file"
    else
        "$@"
    fi
}

run_one() { # run_one ARM CELL REP
    local arm=$1 cell=$2 rep=$3 run="$1-$2-$3" k
    RUN_DIR="$OUT/runs/$run"
    mkdir -p "$RUN_DIR"
    local tv=0
    case "$arm" in
    s_*)
        SHAPE_PHASE=${arm#s_}
        TV_PHASES=("$SHAPE_PHASE")
        ;;
    *_p*)
        tv=1
        # shellcheck disable=SC2207 # the phases are words
        TV_PHASES=($(family_phases "${arm%_p*}")) || { say "VOID  $run: unknown family"; return 1; }
        SHAPE_PHASE=${TV_PHASES[0]}
        ;;
    *)
        say "VOID  $run: unknown arm"
        return 1
        ;;
    esac
    echo "load=$(cut -d' ' -f1 /proc/loadavg) nproc=$(nproc) start=$(date -Is) tv=$tv phase_s=$HOLD" \
        "phases='${TV_PHASES[*]}' spec0='$(spec "$SHAPE_PHASE")' node_pin='${NODE_PIN:-}' est_trace=$EST_TRACE" \
        >"$RUN_DIR/meta.txt"
    if ! topo "$run" >"$RUN_DIR/topo.log" 2>&1; then
        say "VOID  $run: no topology (see $RUN_DIR/topo.log)"
        return 1
    fi
    local t0
    t0=$("$MONO")
    echo "PHASE idx=0 key=$SHAPE_PHASE t_before_ns=$t0 t_after_ns=$t0 ok=1" >"$RUN_DIR/phases.txt"
    local stop="$WORK/stop-$run" mode=be
    rm -f "$stop"
    [ "$cell" != be_small ] && mode=rel
    local common=(-b "$BCAST" -p "$PROFILE" -q "$stop")
    trace_err "$RUN_DIR/idle.err" inns 2 "$WORK/node" idle -c 3 -d $((DUR + 60)) "${common[@]}" \
        >"$RUN_DIR/idle.log" 2>&1 &
    local idle_pid=$!
    TV_PID=""
    if [ "$cell" = rel_large ]; then
        inns 1 "$WORK/large" sub -s "$LARGE_BYTES" -d $((DUR + 8)) -R -b "$BCAST" "${LARGE_PROFILE[@]}" \
            >"$RUN_DIR/echo.log" 2>"$RUN_DIR/echo.err" &
        local echo_pid=$!
        sleep 1
        [ "$tv" = 1 ] && { step_changer & TV_PID=$!; }
        inns 0 "$WORK/large" pub -s "$LARGE_BYTES" -r "$LARGE_HZ" -d "$DUR" -R -b "$BCAST" "${LARGE_PROFILE[@]}" \
            >"$RUN_DIR/ping.log" 2>"$RUN_DIR/ping.err"
        wait "$echo_pid"
    else
        trace_err "$RUN_DIR/echo.err" inns 1 "$WORK/node" echo -c 2 -d $((DUR + 60)) -m "$mode" -s "$SMALL_BYTES" \
            -r "$SMALL_HZ" -l "$RUN_DIR/owd_us.txt" -t "$RUN_DIR/trace_echo.txt" "${common[@]}" \
            >"$RUN_DIR/echo.log" 2>&1 &
        local echo_pid=$!
        sleep 1
        [ "$tv" = 1 ] && { step_changer & TV_PID=$!; }
        trace_err "$RUN_DIR/ping.err" inns 0 "$WORK/node" ping -c 1 -d "$DUR" -m "$mode" -s "$SMALL_BYTES" \
            -r "$SMALL_HZ" -l "$RUN_DIR/rtt_us.txt" -t "$RUN_DIR/trace_ping.txt" "${common[@]}" \
            >"$RUN_DIR/ping.log" 2>&1
        sleep 1
        touch "$stop"
        wait "$echo_pid"
    fi
    if [ -n "$TV_PID" ]; then # the changer ended on its own after two changes; this only reaps it
        wait "$TV_PID" 2>/dev/null
        TV_PID=""
    fi
    sleep 1
    touch "$stop"
    wait "$idle_pid"
    for k in 0 1 2; do counters "${NS[$k]}" "${NODES[$k]}"; done
    for k in 0 1 2; do sudo -n ip netns del "${NS[$k]}"; done
    sudo -n ip netns del "$HUB"
    local line
    line=$(grep -h '^RESULT' "$RUN_DIR/ping.log" "$RUN_DIR/echo.log" | tr '\n' ' ' | cut -c1-300)
    say "$run: phases=$(grep -c '^PHASE' "$RUN_DIR/phases.txt") ${line:-<no RESULT line>}"
}

declare -a NS
declare -a TV_PHASES
for rep in $(seq 1 "$REPS"); do
    for cell in $CELLS; do
        for arm in $ARMS; do
            [ "$rep" -le "$(arm_reps "$arm")" ] || continue
            run_one "$arm" "$cell" "$rep"
        done
    done
done
say "--- reading (x2_step_read.py)"
python3 "$HERE/x2_step_read.py" "$OUT" | tee -a "$SUM"
