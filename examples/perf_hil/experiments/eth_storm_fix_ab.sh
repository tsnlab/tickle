#!/usr/bin/env bash
# eth_storm_fix_ab.sh - the ETHERNET amp writer storm's fix candidates (RESULTS `EWS`), A/B on the PC netns. Written
# and committed before its first run; the reading is eth_storm_fix_read.py (its docstring is the rule set, applied in
# its code).
#
# THE CANDIDATES (no wire change either):
#   F - the per-unit back-off EWS proposed: a unit resent k times waits timeout << min(k - 1, 6) before the timeout
#       releases it again (eth_storm_fix_make.py backoff, applied to main). In test_large_sample's sim it cuts the
#       whole-sample storm (167,013 -> 58,065 resent) but slows the slow reader's repairs (all 11 samples in 8 s
#       against 2 s) and draws a "still waiting" warning there, so it is not the branch's change - measured beside it.
#   G - the branch's change (ab/eth-storm-backoff): the repair timeout runs from the resend or from the reader's last
#       progress, whichever is later - progress = an ACKNACK showing a later large send received than any before.
#       While the reader is still receiving what went before a resend, the resend is behind it, not lost. Sim: 18,429
#       resent in the whole-sample case, every existing test as before.
# TREES, all at once so they share the PC's load rep by rep, made by `prepare` from git objects (no checkout of this
#   repository is touched): M = main (M_SHA), A = main again (the A/A control: nothing differs), F = main + F,
#   G = G_SHA. Forced copies of each (eth_storm_fix_make.py): X<t> = drop 90 (a whole sample lost every 3 s from the
#   60th), P<t> = pin + drop 90 (the timeout pinned at ~1.5 ms, the provocation that stormed main in 10 of 20 forced
#   reps; EWS force1 FP). Each tree's PROVENANCE.txt says what it is.
# PHASES (PROFILE / cells as EWS's census: ETHERNET ge0.1b1 rel_large, 1 MiB RELIABLE at 30 Hz, BANDS=1,
#   NETEM_BASE="limit 100000 delay 100us rate 1gbit", NODE_ENV TT_UDP_OFFLOAD=0, LARGE_RSTATS=1, untraced):
#   1. forced: X and P copies of M A F G, FORCE_REPS (20) reps each, all eight at once; beside them P0 = R4's cells
#      (wifi_repair_ab.sh PROFILE=wifi ARMS="ctl ge10b4" CELLS="rel_small rel_large", R4_REPS (8) reps a tree).
#   2. rounds until LAST_START (never by the data): W = wifi_repair_ab.sh ARMS="ge5b4 ge5b2 tv bc" CELLS=rel_large
#      W_REPS (5) a tree a round, one instance a tree; E = the ETHERNET amp cell, E_REPS (10) a round, two instances a
#      tree (labels M M2 ...). No round starts after LAST_START (default 2026-10-27 03:45 KST); at most MAX_ROUNDS.
# SAMPLE SIZE (worked out before the run). Forced, pinned: main stormed 10 of 20 (force1 FP); 0 of 20 against >= 5
#   of 20 is one-sided Fisher p <= 0.024, against 10 p 0.0002. Natural (E): main's census rate 0 of 495 at load < 2,
#   8 of 1,251 at 2-4, 22 of 654 at >= 4; this run puts ~12 benches on the PC at once (load ~4-10 expected), and ~20
#   E rounds give ~400 reps a tree: at 1-3% M + A should storm ~8-24 times in ~800 reps; 0 of 400 against 8 of 800 is
#   p ~ 0.02. (1 - 0.01)^400 = 2%: a candidate at main's rate would show 0 only by chance. At load < 2 the rate is
#   unmeasurably small (0 of 495), so the natural verdict rests on the loaded reps, read by load band.
# Usage: eth_storm_fix_ab.sh prepare   (M_SHA, G_SHA; TREES_DIR, default ~/ewf_trees; refuses an existing tree)
#        eth_storm_fix_ab.sh run       ([TAG=ewf1] [OUT=~/ewf_results] [FORCE_REPS=20] [R4_REPS=8] [W_REPS=5]
#                                       [E_REPS=10] [LAST_START=epoch] [MAX_ROUNDS=60] [TREES_DIR])
#   run's log ends with "=== eth_storm_fix_ab ended"; the reading is $OUT/$TAG.read.txt; each phase appends a line to
#   $OUT/STATUS.txt.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
TREES_DIR=${TREES_DIR:-$HOME/ewf_trees}
MODE=${1:?prepare or run}

prepare() {
    local m=${M_SHA:?M_SHA=main} g=${G_SHA:?G_SHA=the branch}
    mkdir -p "$TREES_DIR" || return 1
    local t base
    for t in M A F G XM XA XF XG PM PA PF PG; do
        if [ -e "$TREES_DIR/$t" ]; then
            echo "REFUSED: $TREES_DIR/$t exists"
            return 2
        fi
    done
    for t in M A F G XM XA XF XG PM PA PF PG; do
        base=${t: -1}
        mkdir -p "$TREES_DIR/$t" || return 1
        if [ "$base" = G ]; then
            git -C "$REPO" archive "$g" | tar -x -C "$TREES_DIR/$t" || return 1
            echo "tree $t: G_SHA $(git -C "$REPO" rev-parse --short "$g")" >"$TREES_DIR/$t/PROVENANCE.txt"
        else
            git -C "$REPO" archive "$m" | tar -x -C "$TREES_DIR/$t" || return 1
            echo "tree $t: M_SHA $(git -C "$REPO" rev-parse --short "$m")" >"$TREES_DIR/$t/PROVENANCE.txt"
        fi
        if [ "$base" = F ]; then
            python3 -I "$HERE/eth_storm_fix_make.py" backoff "$TREES_DIR/$t" || return 1
            echo "  + backoff (candidate F)" >>"$TREES_DIR/$t/PROVENANCE.txt"
        fi
        case $t in
        P*)
            python3 -I "$HERE/eth_storm_fix_make.py" pin "$TREES_DIR/$t" || return 1
            echo "  + pin" >>"$TREES_DIR/$t/PROVENANCE.txt"
            ;;
        esac
        case $t in
        X* | P*)
            python3 -I "$HERE/eth_storm_fix_make.py" drop "$TREES_DIR/$t" 90 || return 1
            echo "  + drop 90" >>"$TREES_DIR/$t/PROVENANCE.txt"
            ;;
        esac
        echo "  src/tickle.c sha256 $(sha256sum "$TREES_DIR/$t/src/tickle.c" | cut -c1-16)" >>"$TREES_DIR/$t/PROVENANCE.txt"
        cat "$TREES_DIR/$t/PROVENANCE.txt"
    done
    # A and M must be the same source; XA/XM and PA/PM too
    local a b
    for p in "M A" "XM XA" "PM PA"; do
        read -r a b <<<"$p"
        cmp -s "$TREES_DIR/$a/src/tickle.c" "$TREES_DIR/$b/src/tickle.c" || { echo "REFUSED: $a and $b differ"; return 3; }
    done
    echo "prepare done"
}

run() {
    local TAG=${TAG:-ewf1} OUT=${OUT:-$HOME/ewf_results}
    local FORCE_REPS=${FORCE_REPS:-20} R4_REPS=${R4_REPS:-8} W_REPS=${W_REPS:-5} E_REPS=${E_REPS:-10}
    local MAX_ROUNDS=${MAX_ROUNDS:-60} LAST_START=${LAST_START:-$(date -d '2026-10-27 03:45' +%s)}
    mkdir -p "$OUT" || return 1
    local t
    for t in M A F G XM XA XF XG PM PA PF PG; do
        [ -f "$TREES_DIR/$t/PROVENANCE.txt" ] || { echo "REFUSED: $TREES_DIR/$t not prepared"; return 2; }
        cat "$TREES_DIR/$t/PROVENANCE.txt"
    done
    status() { echo "$(date '+%H:%M') ewf: $*" >>"$OUT/STATUS.txt"; echo "$(date -Is) $*"; }
    status "run $TAG start: FORCE_REPS=$FORCE_REPS R4_REPS=$R4_REPS W_REPS=$W_REPS E_REPS=$E_REPS" \
        "LAST_START=$(date -d "@$LAST_START" '+%F %H:%M') load $(cut -d' ' -f1-3 /proc/loadavg)"
    export NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1
    eth_one() { # label tree reps out-subdir
        env OUT="$OUT/${TAG}_$4_$1" REPS="$3" DUR=30 PROFILE=ethernet ARMS="ge0.1b1" CELLS="rel_large" LARGE_HZ=30 \
            BANDS=1 NETEM_BASE="limit 100000 delay 100us rate 1gbit" \
            "$TREES_DIR/$2/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >"$OUT/${TAG}_$4_$1.log" 2>&1
    }
    # ---- phase 1: forced (X, P) and R4's cells, at once
    status "phase 1 (forced X/P x M A F G, $FORCE_REPS reps each; P0 R4 $R4_REPS reps a tree) start"
    local pids=()
    for t in M A F G; do
        eth_one "$t" "X$t" "$FORCE_REPS" x &
        pids+=($!)
        eth_one "$t" "P$t" "$FORCE_REPS" p &
        pids+=($!)
    done
    TREES="M=$TREES_DIR/M A=$TREES_DIR/A F=$TREES_DIR/F G=$TREES_DIR/G" TAG="${TAG}_r4" BASE_OUT="$OUT" \
        PROFILE=wifi REPS="$R4_REPS" ARMS="ctl ge10b4" CELLS="rel_small rel_large" "$HERE/wifi_repair_ab.sh" \
        >/dev/null 2>&1 &
    pids+=($!)
    local p
    for p in "${pids[@]}"; do wait "$p"; done
    status "phase 1 done"
    # ---- phase 2: rounds
    eth_rounds() {
        local r=0 pids=() t
        while [ "$r" -lt "$MAX_ROUNDS" ] && [ "$(date +%s)" -le "$LAST_START" ]; do
            r=$((r + 1))
            echo "E round $r start $(date -Is) load $(cut -d' ' -f1 /proc/loadavg)"
            pids=()
            for t in M A F G; do
                eth_one "$t" "$t" "$E_REPS" "e$r" &
                pids+=($!)
                eth_one "${t}2" "$t" "$E_REPS" "e$r" &
                pids+=($!)
            done
            for p in "${pids[@]}"; do wait "$p"; done
            echo "E round $r done $(date -Is)"
        done
        status "E rounds done: $r"
    }
    wifi_rounds() {
        local r=0
        while [ "$r" -lt "$MAX_ROUNDS" ] && [ "$(date +%s)" -le "$LAST_START" ]; do
            r=$((r + 1))
            echo "W round $r start $(date -Is) load $(cut -d' ' -f1 /proc/loadavg)"
            TREES="M=$TREES_DIR/M A=$TREES_DIR/A F=$TREES_DIR/F G=$TREES_DIR/G" TAG="${TAG}_w$r" BASE_OUT="$OUT" \
                PROFILE=wifi REPS="$W_REPS" ARMS="ge5b4 ge5b2 tv bc" CELLS=rel_large "$HERE/wifi_repair_ab.sh" \
                >/dev/null 2>&1
            echo "W round $r done $(date -Is)"
        done
        status "W rounds done: $r"
    }
    status "phase 2 (rounds) start"
    eth_rounds &
    local eth=$!
    wifi_rounds
    wait "$eth"
    status "all rounds done"
    python3 -I "$HERE/eth_storm_fix_read.py" "$OUT" "$TAG" >"$OUT/$TAG.read.txt" 2>&1
    status "read rc=$? -> $OUT/$TAG.read.txt"
    grep -E '^(CONTROL|LOOP|NATURAL|NO WORSE|R4|LAND|PREFER|VOID)' "$OUT/$TAG.read.txt"
}

T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== eth_storm_fix_ab ended mode=$MODE rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
echo "=== eth_storm_fix_ab $MODE $(date -Is) from $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null) load" \
    "$(cut -d' ' -f1-3 /proc/loadavg) ==="
case $MODE in
prepare) prepare ;;
run) run ;;
*) echo "unknown mode $MODE" && false ;;
esac
