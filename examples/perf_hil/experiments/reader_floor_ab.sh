#!/usr/bin/env bash
# reader_floor_ab.sh - the least-transit floor on the reader's retry estimate (RESULTS `RLF`, 2026-10-26), A/B on the
# PC netns. Written and committed before its first run.
#
# THE CHANGE (F): no recovery is folded into a writer proxy's path estimate (recovery_srtt_ns) as faster than the
# least repair transit the proxy has timed (tt_WriterProxy.transit_min_ns; repair transit = from the OLDEST remembered
# ACKNACK naming a still-missing unit to the arrival of a copy of it addressed to this reader, note_repair_arrival()).
# Every link, either timing rule, the seed included (path_recovery_sample(), tickle.c). The census (`RLC`,
# reader_collapse_census.sh) found every collapse seeded by an ambiguous 3-90 us probe.
#
# TREES, all at once, sharing the PC's load rep by rep: M = origin/main (a5b52eee), A = origin/main in a second
# checkout (the A/A control: the change provably cannot touch it), F = M + the floor. PROFILE=wifi NODE_ENV=
# TT_UDP_OFFLOAD=0 LARGE_RSTATS=1, untraced, as the census's U tree.
#   P0 (R4's cells): wifi_repair_ab.sh ARMS="ctl ge10b4" CELLS="rel_small rel_large", R4_REPS (8) reps a tree. The
#      rel_small ctl gives base_rtt; without it R4 is VOID.
#   W rounds (the census's Wi-Fi cells): wifi_repair_ab.sh ARMS="ge5b4 ge5b2 tv bc" CELLS=rel_large, W_REPS (5) reps,
#      two instances a tree (labels M M2 A A2 F F2), so 2 x 4 x W_REPS reps a tree a round (~14 min). Starts after P0.
#   E rounds (ETHERNET's amp cell, as the census's E1, untraced): PROFILE=ethernet ARMS=ge0.1b1 CELLS=rel_large
#      LARGE_HZ=30 BANDS=1 NETEM_BASE="limit 100000 delay 100us rate 1gbit", E_REPS (10) reps a tree a round (an ETHERNET
#      rep takes ~80 s against Wi-Fi's ~40: ~14 min); runs alongside P0 and the W rounds.
#   STOPPING (fixed before the run, by the clock, never by the data): no round of either kind starts after LAST_START
#   (default 16:30 KST today); a round started runs to its end. At most MAX_ROUNDS (20) of each.
#
# SAMPLE SIZE (worked out before the run). Census rates on main: Wi-Fi 15 COLLAPSE in 990 reps pooled over the four
#   arms (1.5%), ETHERNET amp 6 in 75 (8%). Launched ~12:45; P0 takes ~21 min (32 reps of ~40 s a
#   tree), then ~15 W rounds of ~14 min to 16:30 (2 x 4 x 5 = 40 reps a tree each: ~600 Wi-Fi reps a tree), and
#   alongside ~16 E rounds (~160 ETHERNET reps a tree). Expected on M: ~9 Wi-Fi + ~13 ETHERNET = ~22 COLLAPSE reps. If
#   F removes them, 0 against 22 in ~760 reps a tree gives Fisher p ~ 2e-7; F at half M's rate, p ~ 0.05. Wi-Fi
#   alone: (1 - 0.015)^600 = 0.01%, so a Wi-Fi F of 0 is informative on its own; with ~9 expected on M, a Wi-Fi
#   verdict alone needs M >= 5 (0 of n against 5 of n is one-sided p ~ 0.03). The pooled test is the registered one.
#   If the PC is slower than this (rounds longer, fewer of them), the reading runs on what finished, and with M under
#   the counts the test needs it says UNDECIDED - the rules are not stretched.
#
# READING: reader_floor_read.py (its docstring is the rule set, applied in its code) - CONTROL, COLLAPSE (the census's
#   rule, reader_collapse_read.py, verbatim), NO WORSE on delivered / p50 / p99 / resends / duplicates / CPU per cell,
#   R4, and the LAND line.
# Usage: M=dir A=dir F=dir [TAG=rlf1] [OUT=~/rlf_results] [R4_REPS=8] [W_REPS=5] [E_REPS=10] [LAST_START=epoch]
#        [MAX_ROUNDS=20] reader_floor_ab.sh
#   Output: $OUT/<TAG>_{r4,w<n>,e<n>}_<label>/ and $OUT/<TAG>.read.txt; this script's log ends with
#   "=== reader_floor_ab ended".
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
M=${M:?M=main tree}
A=${A:?A=second main tree}
F=${F:?F=fix tree}
TAG=${TAG:-rlf1}
OUT=${OUT:-$HOME/rlf_results}
R4_REPS=${R4_REPS:-8}
W_REPS=${W_REPS:-5}
E_REPS=${E_REPS:-10}
MAX_ROUNDS=${MAX_ROUNDS:-20}
LAST_START=${LAST_START:-$(date -d 'today 16:30' +%s)}
mkdir -p "$OUT" || exit 1
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== reader_floor_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
echo "=== reader_floor_ab $(date -Is) tag=$TAG R4_REPS=$R4_REPS W_REPS=$W_REPS E_REPS=$E_REPS MAX_ROUNDS=$MAX_ROUNDS" \
    "LAST_START=$(date -d "@$LAST_START" -Is) load $(cut -d' ' -f1-3 /proc/loadavg) ==="
for t in M A F; do
    d=${!t}
    echo "tree $t: $d at $(git -C "$d" rev-parse --short HEAD) dirty $(git -C "$d" status --porcelain -- src include | wc -l)"
done
export NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1

eth_rounds() {
    local r=0 t pids
    while [ "$r" -lt "$MAX_ROUNDS" ] && [ "$(date +%s)" -le "$LAST_START" ]; do
        r=$((r + 1))
        echo "E round $r start $(date -Is) load $(cut -d' ' -f1 /proc/loadavg)"
        pids=()
        for t in M A F; do
            env OUT="$OUT/${TAG}_e${r}_$t" REPS="$E_REPS" DUR=30 PROFILE=ethernet ARMS="ge0.1b1" CELLS="rel_large" \
                LARGE_HZ=30 BANDS=1 NETEM_BASE="limit 100000 delay 100us rate 1gbit" \
                "${!t}/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >"$OUT/${TAG}_e${r}_$t.log" 2>&1 &
            pids+=($!)
        done
        for p in "${pids[@]}"; do wait "$p"; done
        echo "E round $r done $(date -Is)"
    done
}

wifi_rounds() {
    echo "P0 start $(date -Is)"
    TREES="M=$M A=$A F=$F" TAG="${TAG}_r4" BASE_OUT="$OUT" PROFILE=wifi REPS="$R4_REPS" ARMS="ctl ge10b4" \
        CELLS="rel_small rel_large" "$HERE/wifi_repair_ab.sh" >/dev/null 2>&1
    echo "P0 done $(date -Is)"
    local r=0
    while [ "$r" -lt "$MAX_ROUNDS" ] && [ "$(date +%s)" -le "$LAST_START" ]; do
        r=$((r + 1))
        echo "W round $r start $(date -Is) load $(cut -d' ' -f1 /proc/loadavg)"
        TREES="M=$M A=$A F=$F M2=$M A2=$A F2=$F" TAG="${TAG}_w$r" BASE_OUT="$OUT" PROFILE=wifi REPS="$W_REPS" \
            ARMS="ge5b4 ge5b2 tv bc" CELLS=rel_large "$HERE/wifi_repair_ab.sh" >/dev/null 2>&1
        echo "W round $r done $(date -Is)"
    done
}

eth_rounds &
eth=$!
wifi_rounds
wait "$eth"
echo "all rounds done $(date -Is)"
python3 "$HERE/reader_floor_read.py" "$OUT" "$TAG" >"$OUT/$TAG.read.txt" 2>&1
echo "read rc=$?"
grep -E '^(CONTROL|COLLAPSE|NO WORSE|R4|LAND|VOID)' "$OUT/$TAG.read.txt"
