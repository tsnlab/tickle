#!/usr/bin/env bash
# hybrid_pc_ab.sh - does the hybrid transport lower the work per MB of large samples? (DESIGN.md section 8, "Hybrid
# transport", the PC side of steps 3 and 4; the rig campaign is hybrid_rig_task.md.)
#
# CLAIM: with runs, a 1 MB or 4 MB sample costs fewer instructions per MB - user plus kernel, publisher and subscriber -
# than with one MTU fragment per datagram, because most of the cost is per datagram; and RELIABLE at 1% loss is not
# dearer, because repairs stay MTU units.
#
# ARMS, each a commit (ARM_SHAS="name=sha ..."; the first is the reference), each built from its own exported tree with
# its own large_sample_bench.c, as rmw_tickle builds core (tt_MAX_BUFFER_LENGTH 65507, large samples) and with the
# shared-memory transport compiled out (the namespaces share /dev/shm):
#   parent     e1732523, step 2: receives runs, sends MTU fragments only - the reference
#   parent_aa  the same commit built again: its difference from parent is this session's noise, THE FLOOR
#   hybrid     the commit under test, at its defaults (AUTO, the reader's cap on)
#
# TRAFFIC. Publisher and subscriber in two private network namespaces joined by a veth pair, pinned to CPU_PUB and
# CPU_SUB, each under `perf stat -e instructions:u,instructions:k`. Cells: 1 MB at 30 Hz and 4 MB at 15 Hz, RELIABLE
# (KEEP_LAST 10) and BEST_EFFORT, netem loss 0% and 1% on the publisher's egress (per frame: an IP fragment is a
# frame). Arm order rotated per repetition and cell, so drift lands on every arm.
#
# HOW TO READ IT, written before the first run (2026-10-11) and implemented in hybrid_pc_ab.py, not only here:
#   Metric: instructions (user + kernel) per MB published, for each process - the subscriber's per MB offered to it,
#     so a best-effort arm that delivers less is not credited for the work it did not do. Also printed: per MB
#     delivered, delivered/published, torn, latency median and p99 (not scored: a veth), and each arm's realized
#     treatment (its n histogram and fits).
#   Run VOID: no RESULT line from either side; perf output missing; torn != 0; RELIABLE delivered < published - 10
#     (KEEP_LAST may let go of a sample behind a 4 MB send; more than that is a fault, not a cost).
#   Overall VOID: fewer than REPS_MIN (3) valid runs of any (arm, cell); the hybrid arm's AUTO at 0% whose n histogram
#     never reached n_max (the arm did not apply the treatment).
#   Per (cell, process): d = mean over reps of (X / parent - 1), SE over reps. FLOOR = max(1%, |d(parent_aa)| +
#     2 SE(parent_aa)). BETTER: d < -FLOOR and beyond 2 SE. COST: d > FLOOR and beyond 2 SE. Otherwise HELD.
#   Expectation (falsifiable): 0% loss, both sizes, both QoS - hybrid BETTER for both processes. 1% loss: RELIABLE not
#     COST for either process; best effort delivers ~nothing in every arm at 1 MB ((0.99)^723 = 0.07% of samples whole),
#     so its rows read the work of sending, not of delivering.
#
# Usage: hybrid_pc_ab.sh     env: ARM_SHAS (default "parent=e1732523 parent_aa=e1732523 hybrid=HEAD") REPS (5) DUR (6)
#        CPU_PUB (12) CPU_SUB (13) CELLS (all) OUT (default ~/hybrid_pc_ab/<stamp>)
# Needs passwordless `sudo -n ip` and perf (perf_event_paranoid <= 1). Long: launch detached, read $OUT/summary.txt.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
REPS=${REPS:-5}
DUR=${DUR:-6}
CPU_PUB=${CPU_PUB:-12}
CPU_SUB=${CPU_SUB:-13}
ARM_SHAS=${ARM_SHAS:-parent=e1732523 parent_aa=e1732523 hybrid=HEAD}
T0=$(date +%s)
OUTD=${OUT:-$HOME/hybrid_pc_ab/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD/runs" || exit 1
SUMMARY="$OUTD/summary.txt"
: >"$SUMMARY"
say() { echo "$*" | tee -a "$SUMMARY"; }

declare -A SHA_OF
ARMS=""
for pair in $ARM_SHAS; do
    name=${pair%%=*}
    full=$(git -C "$REPO" rev-parse --verify -q "${pair#*=}^{commit}") || {
        echo "no commit ${pair#*=} in $REPO" >&2
        exit 1
    }
    SHA_OF[$name]=$full
    ARMS="$ARMS $name"
done
ARMS=${ARMS# }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/hybrid_pc_ab.XXXXXX") || exit 1
NSP=hab$$
# One EXIT trap: the namespaces (and so every process in them) and the build trees go, whatever ends the run.
# shellcheck disable=SC2329 # invoked by the trap
cleanup() {
    sudo -n ip netns del "$NSP-p" 2>/dev/null
    sudo -n ip netns del "$NSP-s" 2>/dev/null
    [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf "$WORK"
    return 0
}
trap cleanup EXIT
trap 'exit 130' INT TERM

say "=== hybrid_pc_ab $(date -Is): arms '$ARM_SHAS', $REPS reps, ${DUR}s, cpus pub $CPU_PUB sub $CPU_SUB ==="
for arm in $ARMS; do say "    arm $arm = ${SHA_OF[$arm]}"; done

for arm in $ARMS; do
    tree="$WORK/$arm"
    mkdir -p "$tree" && git -C "$REPO" archive "${SHA_OF[$arm]}" | tar -x -C "$tree" || exit 1
    cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -I"$tree/include" -I"$tree/src" -o "$WORK/bench_$arm" "$tree/examples/perf_hil/experiments/large_sample_bench.c" \
        "$tree/src/tickle.c" "$tree/src/encoding.c" "$tree/src/log.c" "$tree/src/hal_linux.c" -lpthread -lm \
        >"$OUTD/build_$arm.log" 2>&1 || {
        say "FATAL: build of $arm failed - $OUTD/build_$arm.log"
        exit 1
    }
    say "    $arm bench sha256 $(sha256sum "$WORK/bench_$arm" | cut -c1-16)"
done

sudo -n ip netns add "$NSP-p" && sudo -n ip netns add "$NSP-s" || exit 1
sudo -n ip link add habp netns "$NSP-p" type veth peer name habs netns "$NSP-s" || exit 1
sudo -n ip -n "$NSP-p" addr add 192.168.10.1/24 dev habp
sudo -n ip -n "$NSP-s" addr add 192.168.10.2/24 dev habs
for ns in "$NSP-p" "$NSP-s"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NSP-p" link set habp up
sudo -n ip -n "$NSP-s" link set habs up

# run_one ARM CELL SIZE RATE QOS LOSS REP
run_one() {
    local arm=$1 cell=$2 size=$3 rate=$4 qos=$5 loss=$6 rep=$7
    local rd="$OUTD/runs/${cell}_${arm}_r$rep" flags=""
    mkdir -p "$rd"
    [ "$qos" = REL ] && flags="-R"
    sudo -n ip netns exec "$NSP-p" tc qdisc del dev habp root 2>/dev/null
    if [ "$loss" != 0 ]; then
        sudo -n ip netns exec "$NSP-p" tc qdisc add dev habp root netem loss "$loss%" || return
    fi
    # shellcheck disable=SC2024,SC2086 # logs written by this shell; flags deliberately word-split
    sudo -n ip netns exec "$NSP-s" taskset -c "$CPU_SUB" perf stat -x, -e instructions:u,instructions:k \
        -o "$rd/sub.perf" -- "$WORK/bench_$arm" sub -s "$size" -d $((DUR + 7)) $flags >"$rd/sub.log" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NSP-p" taskset -c "$CPU_PUB" perf stat -x, -e instructions:u,instructions:k \
        -o "$rd/pub.perf" -- "$WORK/bench_$arm" pub -s "$size" -r "$rate" -d "$DUR" $flags >"$rd/pub.log" 2>&1
    wait "$sub_pid"
    echo "arm=$arm cell=$cell size=$size rate=$rate qos=$qos loss=$loss rep=$rep sha=${SHA_OF[$arm]}" >"$rd/job"
}

# name size rate qos loss
CELL_LIST=(
    "1MB_REL_0 1048576 30 REL 0" "1MB_BE_0 1048576 30 BE 0" "4MB_REL_0 4194304 15 REL 0" "4MB_BE_0 4194304 15 BE 0"
    "1MB_REL_1 1048576 30 REL 1" "1MB_BE_1 1048576 30 BE 1" "4MB_REL_1 4194304 15 REL 1" "4MB_BE_1 4194304 15 BE 1"
)
read -r -a ARM_LIST <<<"$ARMS"
NARMS=${#ARM_LIST[@]}
for rep in $(seq 1 "$REPS"); do
    ci=0
    for spec in "${CELL_LIST[@]}"; do
        read -r cell size rate qos loss <<<"$spec"
        if [ -n "${CELLS:-}" ]; then
            case " $CELLS " in *" $cell "*) ;; *) continue ;; esac
        fi
        for k in $(seq 0 $((NARMS - 1))); do
            run_one "${ARM_LIST[$(((k + rep + ci) % NARMS))]}" "$cell" "$size" "$rate" "$qos" "$loss" "$rep"
        done
        ci=$((ci + 1))
    done
    say "    rep $rep/$REPS done at $(($(date +%s) - T0)) s"
done

python3 "$HERE/hybrid_pc_ab.py" "$OUTD" "$ARMS" | tee -a "$SUMMARY"
say "=== done in $(($(date +%s) - T0)) s: $OUTD ==="
