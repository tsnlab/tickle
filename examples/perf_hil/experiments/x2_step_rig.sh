#!/usr/bin/env bash
# x2_step_rig.sh - X2 "step response" on the rig: the two Raspberry Pis over the rig's eth0 link, tc netem on both Pis'
# egress, instead of x2_step_netns.sh's namespaces on the PC (the user, 2026-11-10: every experiment is judged on the rig).
#
# Same design, arms, rules and reader as x2_step_netns.sh (x2_step_read.py reads the output unchanged):
#   - ping node on the client Pi (10.1.1.214), echo and idle nodes on the server Pi (10.1.1.213); the data link is eth0
#     (192.168.10.x), broadcast 192.168.10.255; ssh and scp use the Pis' other interface, which is not shaped;
#   - a run is DUR = 3 x HOLD + LEAD s; the first phase, one step, the step back (family D: 1 -> 20 -> 1 ms delay with
#     D_BG_LOSS random loss; LB: 0 -> 5% -> 0 loss, 1 ms delay); statics run the same DUR; arms D_p<HOLD>, LB_p<HOLD>,
#     s_<phase>. Both Pis carry the netem words (as each namespace did), so the round trip carries them twice;
#   - the phase stamps (phases.txt) are the CLIENT Pi's CLOCK_MONOTONIC - the clock the ping node's trace uses - taken just
#     before and after the client's `tc qdisc change`; the server's change runs in parallel from here, and the two are
#     within an ssh round trip of each other, far below HOLD.
# Differences from the netns harness, to state when quoting a result: two machines and a real NIC (not a bridge of veths);
# the prio bands as the netem harness (broadcast band 10, unicast band 20; counters.txt p = client Pi, e = server Pi); the nodes are built on each Pi (-O2, the same
# words) from the tree in SRC, which is copied there, so the Pis' ~/tickle checkout is not touched; no CPU pin list
# (ping on CPU 2 of the client, echo on CPU 2 and idle on CPU 3 of the server); the families are D and LB (J is not run
# on the rig: its reorder behaviour depends on netem on one box); cells be_small, rel_small and rel_large (1 MiB at 5 Hz,
# large_sample_bench pub on the client Pi and sub on the server Pi, as the netns harness).
#
# Usage (detached, from the harness checkout; SRC = the tree to measure, default this checkout):
#   TS=$(date +%Y%m%d-%H%M%S); setsid nohup env SRC=<tree> OUT=$HOME/rig_results_safe/time_varying/x2rig_$TS \
#       examples/perf_hil/experiments/x2_step_rig.sh > $HOME/rig_results_safe/x2rig_$TS.launch.log 2>&1 </dev/null &
# Env: HOLD=10 LEAD=2 REPS=8 FAMILIES="D LB" CELLS="be_small rel_small" D_BG_LOSS=2 EST_TRACE=1 EXTRA_CC= PROFILE=ethernet
# Duration with the defaults: about 75 min (96 runs of ~45 s). The rig lock is taken here; the EXIT trap removes both
# Pis' qdiscs and stops the nodes this script started.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
SRC=${SRC:-$REPO}
HOLD=${HOLD:-10}
LEAD=${LEAD:-2}
REPS=${REPS:-8}
FAMILIES=${FAMILIES:-"D LB"}
CELLS=${CELLS:-"be_small rel_small"}
PROFILE=${PROFILE:-ethernet}
D_BG_LOSS=${D_BG_LOSS:-2}
EST_TRACE=${EST_TRACE:-1}
NODE_ENV=${NODE_ENV-TT_UDP_OFFLOAD=0} # one datagram per skb, as x2_step_netns.sh: the qdisc counters then count what the NIC sends
SMALL_HZ=100
SMALL_BYTES=100
BCAST=192.168.10.255
K=$HOME/.ssh/tickle_ci_ed25519
CLIENT=10.1.1.214
SERVER=10.1.1.213
RD=/tmp/x2rig
[[ $HOLD =~ ^[0-9]+$ && $LEAD =~ ^[0-9]+$ && $REPS =~ ^[0-9]+$ ]] || { echo "HOLD, LEAD, REPS are integers" >&2; exit 2; }
case "$EST_TRACE" in 0 | 1) ;; *) echo "EST_TRACE must be 0 or 1" >&2; exit 2 ;; esac
for c in $CELLS; do case "$c" in be_small | rel_small | rel_large) ;; *) echo "unknown cell $c" >&2; exit 2 ;; esac; done
LARGE_BYTES=1048576
LARGE_HZ=${LARGE_HZ:-5}
DUR=$((3 * HOLD + LEAD))
TRACE_DEF=""
[ "$EST_TRACE" = 1 ] && TRACE_DEF="-Dtt_ESTIMATOR_TRACE"
OUT=${OUT:-$HOME/rig_results_safe/time_varying/x2rig_$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT/runs" || exit 1
SUM="$OUT/summary.txt"
say() { echo "$*" | tee -a "$SUM"; }
if [ "${READ_ONLY:-0}" = 1 ]; then
    python3 "$HERE/x2_step_read.py" "$OUT"
    exit $?
fi
: >"$SUM"

sh_() {
    local h=$1
    shift
    ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "$@" </dev/null
}
fetch_() { scp -q -i "$K" -o BatchMode=yes "ci@$1:$2" "$3" 2>/dev/null; }

family_phases() {
    case "$1" in
    D) echo "d1 d20" ;;
    LB) echo "l0 lb5" ;;
    *) return 1 ;;
    esac
}
STATICS="s_l0"
TV_ARMS=""
for fam in $FAMILIES; do
    phases=$(family_phases "$fam") || { echo "unknown family $fam (D, LB)" >&2; exit 2; }
    TV_ARMS="$TV_ARMS ${fam}_p$HOLD"
    for ph in $phases; do
        case " $STATICS " in *" s_$ph "*) ;; *) STATICS="$STATICS s_$ph" ;; esac
    done
done
ARMS="$STATICS$TV_ARMS"

spec() {
    local words
    case "$1" in
    d1) words="delay 1ms loss random ${D_BG_LOSS}%" ;;
    d20) words="delay 20ms loss random ${D_BG_LOSS}%" ;;
    l0) words="delay 1ms" ;;
    lb5) words="delay 1ms loss random 5%" ;;
    *) return 1 ;;
    esac
    echo "limit 10000 $words"
}

stop_all() {
    local h
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "sudo -n tc qdisc del dev eth0 root 2>/dev/null; for f in $RD/*.pid; do [ -s \$f ] || continue; p=\$(cat \$f); case \"\$(readlink /proc/\$p/exe 2>/dev/null)\" in $RD/*) kill -INT \$p ;; esac; done; true" >/dev/null 2>&1
    done
}
on_exit() {
    local rc=$?
    [ -n "${CH_PID:-}" ] && kill "$CH_PID" 2>/dev/null
    stop_all
    say "=== x2_step_rig ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT

# refuse when someone's shaping is still on eth0
for h in "$CLIENT" "$SERVER"; do
    q=$(sh_ "$h" "tc qdisc show dev eth0") || { say "VOID  cannot reach $h"; exit 1; }
    case "$q" in *netem* | *tbf*) say "REFUSED: $h eth0 carries a shaper: $q"; exit 1 ;; esac
done

TREE_SHA=$(git -C "$SRC" rev-parse --short HEAD)
DIRTY=$(git -C "$SRC" status --porcelain -- src include platform | wc -l)
say "=== x2_step_rig $(date -Is): tree=$TREE_SHA dirty_paths=$DIRTY harness=$(git -C "$REPO" rev-parse --short HEAD) REPS=$REPS HOLD=$HOLD LEAD=$LEAD DUR=$DUR ARMS='$ARMS' CELLS='$CELLS' d_bg_loss=$D_BG_LOSS est_trace=$EST_TRACE ==="
{
    echo "tree=$TREE_SHA dirty_paths=$DIRTY profile=$PROFILE reps=$REPS dur=$DUR d_bg_loss=$D_BG_LOSS"
    echo "x2=1 hold=$HOLD lead=$LEAD j_reps=0 arms='$ARMS' cells='$CELLS' node_pin='rig: client cpu2, server cpu2 and cpu3' node_env=''"
    echo "small_bytes=$SMALL_BYTES small_hz=$SMALL_HZ large_bytes=0 large_hz=0 rig=client:$CLIENT,server:$SERVER"
    echo "est_trace=$EST_TRACE"
    echo "build_small=cc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS ${EXTRA_CC:-}${TRACE_DEF:+ $TRACE_DEF}"
} >>"$OUT/provenance.txt"

# --- build on each Pi from the tree in SRC (copied to $RD/src) --------------------------------------------------------
cat >"$OUT/mono.c" <<'EOF'
#include <stdio.h>
#include <time.h>
int main(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("%llu\n", (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec);
    return 0;
}
EOF
build_on() {
    local h=$1
    sh_ "$h" "rm -rf $RD && mkdir -p $RD/src $RD/runs" || return 1
    tar -C "$SRC" -cf - src include examples/perf_hil/tickle/common examples/perf_hil/experiments/wifi_link_node.c examples/perf_hil/experiments/large_sample_bench.c |
        ssh -i "$K" -o BatchMode=yes "ci@$h" "tar -C $RD/src -xf -" || return 1
    scp -q -i "$K" -o BatchMode=yes "$OUT/mono.c" "ci@$h:$RD/mono.c" || return 1
    sh_ "$h" "cd $RD/src && cc -O2 -DNDEBUG -Wall -Wextra -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS ${EXTRA_CC:-} $TRACE_DEF -Iinclude -Isrc -Iexamples/perf_hil/tickle/common -o $RD/node examples/perf_hil/experiments/wifi_link_node.c src/tickle.c src/encoding.c src/log.c src/hal_linux.c -lpthread -lm && cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS -DR4_TRACE $TRACE_DEF -Iinclude -Isrc -Iexamples/perf_hil/tickle/common -o $RD/large examples/perf_hil/experiments/large_sample_bench.c src/tickle.c src/encoding.c src/log.c src/hal_linux.c -lpthread -lm && cc -O2 -o $RD/mono $RD/mono.c"
}
build_on "$CLIENT" >"$OUT/build_client.log" 2>&1 &
b1=$!
build_on "$SERVER" >"$OUT/build_server.log" 2>&1 &
b2=$!
if ! wait "$b1" || ! wait "$b2"; then
    say "VOID  a Pi did not build the node (see $OUT/build_*.log)"
    exit 1
fi

# shape_all PHASE: on both Pis' eth0 a prio root with the broadcast band 1:1 and the unicast band 1:2, each a netem with
# the phase's words (as x2_step_netns.sh): the reader's realized unicast loss reads band 20's counters, and every run
# starts from a fresh root so the counters are the run's own.
shape_all() {
    local words h
    words=$(spec "$1") || return 1
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "sudo -n tc qdisc del dev eth0 root 2>/dev/null; set -e
            sudo -n tc qdisc add dev eth0 root handle 1: prio bands 2 priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
            sudo -n tc qdisc add dev eth0 parent 1:1 handle 10: netem $words
            sudo -n tc qdisc add dev eth0 parent 1:2 handle 20: netem $words
            sudo -n tc filter add dev eth0 parent 1: protocol ip prio 1 u32 match ip dst $BCAST/32 flowid 1:1
            sudo -n tc filter add dev eth0 parent 1: protocol ip prio 1 u32 match ip dst 255.255.255.255/32 flowid 1:1" ||
            return 1
    done
}
# counters LABEL HOST: the host's eth0 counters and qdisc statistics in the layout counters_of() reads
# The NIC's counters run from boot, so a run's tx is the difference from the stamp taken when its shaping was set (TX0).
txstat() { sh_ "$1" "cat /sys/class/net/eth0/statistics/tx_bytes /sys/class/net/eth0/statistics/tx_packets" | tr '\n' ' '; }
declare -A TX0
counters() {
    local label=$1 h=$2 now b0 p0
    now=$(txstat "$h")
    read -r b0 p0 <<<"${TX0[$label]}"
    # shellcheck disable=SC2086 # two words
    set -- $now
    {
        echo "== $label tx_bytes=$(($1 - b0)) tx_packets=$(($2 - p0)) tx_dropped=0 rx_dropped=0"
        sh_ "$h" "tc -s qdisc show dev eth0"
    } >>"$RUN_DIR/counters.txt"
}
change_all() { # PHASE: both Pis at once; prints the client's monotonic stamps around its own change
    local words stamps
    words=$(spec "$1") || return 1
    sh_ "$SERVER" "sudo -n tc qdisc change dev eth0 parent 1:1 handle 10: netem $words; sudo -n tc qdisc change dev eth0 parent 1:2 handle 20: netem $words" &
    local sp=$!
    stamps=$(sh_ "$CLIENT" "a=\$($RD/mono); sudo -n tc qdisc change dev eth0 parent 1:1 handle 10: netem $words; sudo -n tc qdisc change dev eth0 parent 1:2 handle 20: netem $words; rc=\$?; b=\$($RD/mono); echo \$a \$b \$rc")
    wait "$sp"
    echo "$stamps"
}
step_changer() {
    local idx=0 key st
    sleep "$LEAD"
    for key in "${TV_PHASES[1]}" "${TV_PHASES[0]}"; do
        sleep "$HOLD"
        idx=$((idx + 1))
        st=$(change_all "$key")
        # shellcheck disable=SC2086 # three words
        set -- $st
        echo "PHASE idx=$idx key=$key t_before_ns=${1:-0} t_after_ns=${2:-0} ok=$([ "${3:-1}" = 0 ] && echo 1 || echo 0)" \
            >>"$RUN_DIR/phases.txt"
    done
}

run_one() { # ARM CELL REP
    local arm=$1 cell=$2 rep=$3 run="$1-$2-$3" mode=be tv=0 t0
    RUN_DIR="$OUT/runs/$run"
    mkdir -p "$RUN_DIR"
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
    [ "$cell" != be_small ] && mode=rel
    echo "start=$(date -Is) tv=$tv phase_s=$HOLD phases='${TV_PHASES[*]}' spec0='$(spec "$SHAPE_PHASE")' rig=1 est_trace=$EST_TRACE" \
        >"$RUN_DIR/meta.txt"
    if ! shape_all "$SHAPE_PHASE"; then
        say "VOID  $run: cannot shape"
        return 1
    fi
    TX0[p]=$(txstat "$CLIENT")
    TX0[e]=$(txstat "$SERVER")
    local trace_e="" trace_p=""
    [ "$EST_TRACE" = 1 ] && { trace_e="2>$RD/echo.err"; trace_p="2>$RD/ping.err"; }
    # server: echo (CPU 2) and idle (CPU 3), each under its pid file; -q is the stop file the nodes poll
    local echo_cmd="$RD/node echo -c 2 -d $((DUR + 60)) -m $mode -s $SMALL_BYTES -r $SMALL_HZ -l $RD/owd_us.txt -t $RD/trace_echo.txt -b $BCAST -p $PROFILE -q $RD/stop"
    local ping_cmd="$RD/node ping -c 1 -d $DUR -m $mode -s $SMALL_BYTES -r $SMALL_HZ -l $RD/rtt_us.txt -t $RD/trace_ping.txt -b $BCAST -p $PROFILE -q $RD/stop"
    if [ "$cell" = rel_large ]; then
        echo_cmd="$RD/large sub -s $LARGE_BYTES -d $((DUR + 8)) -R -b $BCAST"
        ping_cmd="$RD/large pub -s $LARGE_BYTES -r $LARGE_HZ -d $DUR -R -b $BCAST"
        trace_e="2>$RD/echo.err"
        trace_p="2>$RD/ping.err"
    fi
    sh_ "$SERVER" "rm -f $RD/stop $RD/*.pid $RD/*.log $RD/*.err $RD/*.txt; \
        setsid nohup env $NODE_ENV taskset -c 3 $RD/node idle -c 3 -d $((DUR + 60)) -b $BCAST -p $PROFILE -q $RD/stop >$RD/idle.log 2>$RD/idle.err </dev/null & echo \$! >$RD/idle.pid; \
        setsid nohup env $NODE_ENV taskset -c 2 $echo_cmd >$RD/echo.log ${trace_e:-2>&1} </dev/null & echo \$! >$RD/echo.pid; true" ||
        { say "VOID  $run: cannot start the server nodes"; return 1; }
    sleep 3
    sh_ "$CLIENT" "rm -f $RD/*.log $RD/*.err $RD/*.txt $RD/stop" >/dev/null
    t0=$(sh_ "$CLIENT" "$RD/mono")
    echo "PHASE idx=0 key=$SHAPE_PHASE t_before_ns=$t0 t_after_ns=$t0 ok=1" >"$RUN_DIR/phases.txt"
    CH_PID=""
    [ "$tv" = 1 ] && { step_changer & CH_PID=$!; }
    sh_ "$CLIENT" "env $NODE_ENV taskset -c 2 $ping_cmd >$RD/ping.log ${trace_p:-2>&1}"
    sleep 1
    if [ -n "$CH_PID" ]; then
        wait "$CH_PID" 2>/dev/null
        CH_PID=""
    fi
    : >"$RUN_DIR/counters.txt"
    counters p "$CLIENT"
    counters e "$SERVER"
    if [ "$cell" = rel_large ]; then
        sh_ "$SERVER" "p=\$(cat $RD/echo.pid); n=0; while [ -d /proc/\$p ] && [ \$n -lt 30 ]; do sleep 1; n=\$((n+1)); done; true" >/dev/null
    fi
    sh_ "$SERVER" "touch $RD/stop" >/dev/null
    sleep 2
    sh_ "$SERVER" "for f in $RD/*.pid; do p=\$(cat \$f); case \"\$(readlink /proc/\$p/exe 2>/dev/null)\" in $RD/*) kill -INT \$p ;; esac; done; true" >/dev/null
    sleep 1
    local f
    for f in rtt_us.txt trace_ping.txt ping.log ping.err; do fetch_ "$CLIENT" "$RD/$f" "$RUN_DIR/$f"; done
    for f in owd_us.txt trace_echo.txt echo.log echo.err idle.log; do fetch_ "$SERVER" "$RD/$f" "$RUN_DIR/$f"; done
    local line
    line=$(grep -h '^RESULT' "$RUN_DIR/ping.log" "$RUN_DIR/echo.log" 2>/dev/null | tr '\n' ' ' | cut -c1-300)
    say "$run: phases=$(grep -c '^PHASE' "$RUN_DIR/phases.txt") ${line:-<no RESULT line>}"
}

declare -a TV_PHASES
for rep in $(seq 1 "$REPS"); do
    for cell in $CELLS; do
        for arm in $ARMS; do
            run_one "$arm" "$cell" "$rep"
        done
    done
done
stop_all
say "--- reading (x2_step_read.py)"
python3 "$HERE/x2_step_read.py" "$OUT" | tee -a "$SUM"
