#!/usr/bin/env bash
# fastdds_be_delivery.sh - why did Fast DDS's BEST_EFFORT reader receive ~0% same-host at KEEP_LAST 1?
#
# The user, 2026-10-07: "FastDDS 0%는 뭔가 문제가 있는 것 같아. 원인 분석이 필요해." In
# fair_samehost_9f919c8b_20261006-204050 (best_effort_throughput p2-p4, -K 1 on client and server) Fast DDS's reader
# took ~200 of ~5.5M samples in 9 s in BOTH arms. The question, the hypothesis, the arms and the reading rules are in
# fastdds_be_delivery.py, which implements them - read it before this file; the rules live in code, not here.
#
# In short: data-sharing is a QoS, not a transport, so the eth0-only "OFF" arm kept it (its server ran a dds.dsha
# thread and put 0 packets per sample on lo). The arms separate data-sharing (F3 turns it off), the writer's pool
# depth (F2), the rate (F4), and Fast DDS's own account of what it discarded (F1w); TickLE p3 is the control.
#
# Every arm is one s6_transport_cells.sh call (validated, and the source of the 2026-10-06 figures), so the bench, the
# server stop, the /proc readings and the identity checks are the ones that produced the result being questioned.
#
# THE LOCK is taken once, here, for the whole run (exec rig_lock.sh unless RIG_LOCK_HELD_HIL=1); the inner calls see
# RIG_LOCK_HELD_HIL=1 and do not take it again. Preflight: only the TickLE control can be preflighted on this PC (Fast
# DDS 2.14 is not installed here), so that is preflighted before the lock; the Fast DDS side is checked by a one-rep
# DRY RUN on the rig, under the lock, of the arm with the most moving parts (F1w: new server fields, the warning
# counter and its environment crossing ssh). If the dry run is not OK the campaign does not start.
#
# SHA must be a PUSHED commit: s6_transport_cells.sh resets the rig's checkout to it and builds there.
#
# Rig time, estimated before any run: each Fast DDS call = build (~1 min) + REPS x ~31 s (both of s6's arms) ~ 2.5 min;
# five calls ~ 12.5 min; the TickLE control ~ 2 min; the dry run ~ 1.5 min. About 16 min; schedule the 1.5x check at
# 24 min.
#
# Usage: SHA=<pushed sha> fastdds_be_delivery.sh       Output: $OUTB.{dry,T,F1,F2,F3,F4,F1w}.txt, $OUTB.verdicts.txt
#   REPS=3  DUR=5  SIZE=p3  F4_INTERVAL=0.000016  DRY_ONLY=1 (stop after the dry run)
#   ANALYSE_ONLY=<OUTB>  print the verdicts for an existing run; touches no host and takes no lock
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
EXP="$REPO/examples/perf_hil/experiments"
REPS=${REPS:-3}
DUR=${DUR:-5}
SIZE=${SIZE:-p3}
# 10% of F1's ~620k samples/s would be one write every 16 us. nanosleep on the Pi overshoots that, so the realized rate
# will be lower; the rule reads the realized rate (<= 20% of F1's), not this number.
F4_INTERVAL=${F4_INTERVAL:-0.000016}

if [ -n "${ANALYSE_ONLY:-}" ]; then
    exec python3 "$EXP/fastdds_be_delivery.py" "$ANALYSE_ONLY"
fi

SHA=${SHA:?set SHA to a pushed commit - the rig checks it out}
case "$REPS" in '' | *[!0-9]* | 0*) echo "REPS='$REPS' is not a positive integer" >&2; exit 2 ;; esac

export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    if [ "${PREFLIGHT:-1}" != 0 ]; then
        if ! SHA=$SHA PREFLIGHT_TOPO=samens FWS=tickle \
            "$EXP/rig_preflight.sh" "best_effort_throughput:$SIZE:N0:-K 1:-Q"; then
            echo "REFUSING TO TAKE THE RIG: rig_preflight.sh failed (above). PREFLIGHT=0 overrides." >&2
            exit 1
        fi
    fi
    export PREFLIGHT=0
    exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
export PREFLIGHT=0

OUTB=${OUTB:-$HOME/rig_results_safe/fastdds_be_delivery_${SHA}_$(date +%Y%m%d-%H%M%S)}
LOG="$OUTB.driver.log"
mkdir -p "$(dirname "$OUTB")"
: >"$LOG"
say() { echo "$*" | tee -a "$LOG"; }

# cell <tag> <frameworks> <reps> <dur> <depth> [VAR=value ...]: one s6_transport_cells.sh call into $OUTB.<tag>.txt.
# The extra variables are set for that call only (env), so one arm's treatment cannot leak into the next.
cell() {
    local tag=$1 fws=$2 reps=$3 dur=$4 depth=$5
    shift 5
    local out="$OUTB.$tag.txt"
    say "--- $tag: FRAMEWORKS='$fws' REPS=$reps DUR=$dur BE_HISTORY=$depth $* -> $out ($(date +%T))"
    env "$@" FRAMEWORKS="$fws" SCEN=best_effort_throughput SIZE="$SIZE" DUR="$dur" REPS="$reps" SHA="$SHA" \
        BE_HISTORY="$depth" OUT="$out" "$EXP/s6_transport_cells.sh" >"$out.log" 2>&1
    say "    exit $? ($(date +%T))"
}

say "=== fastdds_be_delivery $(date -Is) sha=$SHA size=$SIZE reps=$REPS dur=${DUR}s F4_INTERVAL=$F4_INTERVAL ==="

# DRY RUN: one rep of F1w. Exercises the build at $SHA, the new server fields, the warning counter and its crossing.
cell dry fastdds 1 2 1 BENCH_FASTDDS_COUNT_WARNINGS=1
if ! python3 "$EXP/fastdds_be_delivery.py" --dry "$OUTB.dry.txt" 2>&1 | tee -a "$LOG"; then
    say "=== ABORTED after the dry run $(date -Is) - see $OUTB.dry.txt and $OUTB.dry.txt.log ==="
    exit 1
fi
if [ "${DRY_ONLY:-0}" = 1 ]; then
    say "=== DRY_ONLY: stopping after the dry run $(date -Is) ==="
    exit 0
fi

# The control first, so a rig that is not in yesterday's state shows before any Fast DDS number is read.
cell T tickle "$REPS" "$DUR" 1
cell F1 fastdds "$REPS" "$DUR" 1
cell F2 fastdds "$REPS" "$DUR" 64
cell F3 fastdds "$REPS" "$DUR" 1 BENCH_FASTDDS_NO_DATASHARING=1
cell F4 fastdds "$REPS" "$DUR" 1 CLI_ARGS="-i $F4_INTERVAL"
cell F1w fastdds "$REPS" "$DUR" 1 BENCH_FASTDDS_COUNT_WARNINGS=1

REPS=$REPS python3 "$EXP/fastdds_be_delivery.py" "$OUTB" 2>&1 | tee "$OUTB.verdicts.txt" | tee -a "$LOG"
say "=== ALL_DONE $(date -Is) ==="
