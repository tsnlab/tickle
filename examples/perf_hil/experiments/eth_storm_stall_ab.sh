#!/usr/bin/env bash
# eth_storm_stall_ab.sh - the writer storm's stall follow-up (RESULTS `EWF`), A/B on the PC netns. Written and
# committed before its first run; the reading is eth_storm_stall_read.py (its docstring is the rule set, applied in its
# code).
#
# THE CHANGE (S, branch ab/eth-storm-stall; no wire change): on top of G (the repair timeout runs from the resend or
#   from the reader's last progress, whichever is later), the timeout doubles for each resend of a unit in a row with no
#   progress shown between, at most x64, and any progress since a resend clears it. G left a stall in which the reader
#   shows no progress to the plain timeout: in EWF's 03:33 rep, where every bench on the PC stormed at once, G resent
#   8.5 M (8.47 M of it released by the timeout). Sim (test_large_sample): a reader stalled 80 ms on a whole sample's
#   resend got 30 copies of each unit from G, 6 from S; the slow reader's repairs and the warning test as G's.
# TREES, all at once so they share the PC's load rep by rep, made by `prepare` from git objects (no checkout is
#   touched): M = G (M_SHA, the parent), A = G again (the A/A control: nothing differs), S = S_SHA. Forced copies of
#   each: X<t> = drop 90 (a whole sample lost every 3 s from the 60th; eth_storm_fix_make.py drop), P<t> = pin + drop
#   90 (the timeout pinned at ~1.5 ms: main's forced loop, which G breaks - kept so S is seen not to lose that),
#   Q<t> = drop 90 + stall STALL_MS (100): the reader takes in no large data for 100 ms after the ACKNACK that asks for
#   the whole sample - the stall falls while that sample's resend is pending (eth_storm_stall_make.py stall). Each
#   tree's PROVENANCE.txt says what it is.
# PHASES (cells as EWS's census: ETHERNET ge0.1b1 rel_large, 1 MiB RELIABLE at 30 Hz, BANDS=1,
#   NETEM_BASE="limit 100000 delay 100us rate 1gbit", NODE_ENV TT_UDP_OFFLOAD=0, LARGE_RSTATS=1, untraced):
#   0. quiet: no rep starts before /home/semih/rig_results_safe/ews_queue.log holds "EWS QUEUE DONE" (the rig's Array
#      cells, run from this PC) and the 1-minute load is below 4 (QUIET_LOAD); checked every minute, given up at
#      QUIET_DEADLINE (default 2026-10-27 12:00 KST) - then nothing runs and the log says NOT QUIET.
#   1. forced: X, P and Q copies of M A S, FORCE_REPS (20) reps each, all nine at once; beside them P0 = R4's cells
#      (wifi_repair_ab.sh PROFILE=wifi ARMS="ctl ge10b4" CELLS="rel_small rel_large", R4_REPS (8) reps a tree).
#   2. rounds until LAST_START (never by the data): W = wifi_repair_ab.sh ARMS="ge5b4 ge5b2 tv bc" CELLS=rel_large
#      W_REPS (5) a tree a round; E = the ETHERNET amp cell, E_REPS (10) a round, two instances a tree (M M2 ...).
#      No round starts after LAST_START (default 2026-10-27 14:10 KST); at most MAX_ROUNDS.
#   Every rep's start load is in its meta.txt (wifi_burst_loss_netns.sh); each round's start load is in the log.
# SAMPLE SIZE (before the run). Stall (Q): a rep holds 10 forced losses, so 10 stalls. A smoke rep of G's Q tree
#   (08:29, load 2.0, 1 rep, the stall then 80 ms): 10 stalls of ~5,000 large datagrams dropped each, 66,712 resent,
#   34,386 of them timeout releases at timeouts of 8-16 ms - below the STORM line, so the stall verdict will most
#   likely rest on the continuous rule (Q's median resends, band = the A/A spread, floor 10%). At a 10 ms timeout a
#   100 ms stall lets G release each unit ~10 times, S 3 (at 1, 3 and 7 timeouts); the stall was set to 100 ms
#   (the lead's 50-100 ms) after that smoke rep, before any rep of S. 20 reps a tree: 0 of 20 against >= 5 of 20 is
#   one-sided Fisher p <= 0.024 should G's Q reps storm. Natural (E): EWF saw 4-5 storms in 1,040 reps a tree over
#   6 h, all but the 03:33 rep at load >= 4; ~4 h of rounds give ~600 reps a tree, so the natural verdict is expected
#   UNDECIDED, as EWF's was - read by load band, not decided by it.
# Usage: eth_storm_stall_ab.sh prepare   (M_SHA, S_SHA; TREES_DIR, default ~/ews_stall_trees; refuses an existing tree)
#        eth_storm_stall_ab.sh run       ([TAG=ess1] [OUT=~/ews_stall_results] [FORCE_REPS=20] [R4_REPS=8]
#                                         [W_REPS=5] [E_REPS=10] [STALL_MS - prepare only] [LAST_START=epoch]
#                                         [MAX_ROUNDS=60] [QUIET_LOAD=4] [QUIET_DEADLINE=epoch] [TREES_DIR])
#   run's log ends with "=== eth_storm_stall_ab ended"; the reading is $OUT/$TAG.read.txt; each phase appends a line to
#   $OUT/STATUS.txt.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
TREES_DIR=${TREES_DIR:-$HOME/ews_stall_trees}
MODE=${1:?prepare or run}
ARMS_ALL="M A S XM XA XS PM PA PS QM QA QS"

prepare() {
    local m=${M_SHA:?M_SHA=the parent G} s=${S_SHA:?S_SHA=the change} stall_ms=${STALL_MS:-100}
    mkdir -p "$TREES_DIR" || return 1
    local t base sha
    for t in $ARMS_ALL; do
        if [ -e "$TREES_DIR/$t" ]; then
            echo "REFUSED: $TREES_DIR/$t exists"
            return 2
        fi
    done
    for t in $ARMS_ALL; do
        base=${t: -1}
        sha=$m
        [ "$base" = S ] && sha=$s
        mkdir -p "$TREES_DIR/$t" || return 1
        git -C "$REPO" archive "$sha" | tar -x -C "$TREES_DIR/$t" || return 1
        echo "tree $t: $(git -C "$REPO" rev-parse --short "$sha")" >"$TREES_DIR/$t/PROVENANCE.txt"
        case $t in
        P*)
            python3 -I "$HERE/eth_storm_fix_make.py" pin "$TREES_DIR/$t" || return 1
            echo "  + pin" >>"$TREES_DIR/$t/PROVENANCE.txt"
            ;;
        esac
        case $t in
        X* | P* | Q*)
            python3 -I "$HERE/eth_storm_fix_make.py" drop "$TREES_DIR/$t" 90 || return 1
            echo "  + drop 90" >>"$TREES_DIR/$t/PROVENANCE.txt"
            ;;
        esac
        case $t in
        Q*)
            python3 -I "$HERE/eth_storm_stall_make.py" stall "$TREES_DIR/$t" "$stall_ms" || return 1
            echo "  + stall $stall_ms ms" >>"$TREES_DIR/$t/PROVENANCE.txt"
            ;;
        esac
        echo "  src/tickle.c sha256 $(sha256sum "$TREES_DIR/$t/src/tickle.c" | cut -c1-16)" >>"$TREES_DIR/$t/PROVENANCE.txt"
        cat "$TREES_DIR/$t/PROVENANCE.txt"
    done
    # A and M must be the same source, in every phase; S must differ from M
    local a b
    for p in "M A" "XM XA" "PM PA" "QM QA"; do
        read -r a b <<<"$p"
        cmp -s "$TREES_DIR/$a/src/tickle.c" "$TREES_DIR/$b/src/tickle.c" || { echo "REFUSED: $a and $b differ"; return 3; }
    done
    if cmp -s "$TREES_DIR/M/src/tickle.c" "$TREES_DIR/S/src/tickle.c"; then
        echo "REFUSED: S is M"
        return 3
    fi
    echo "prepare done"
}

run() {
    local TAG=${TAG:-ess1} OUT=${OUT:-$HOME/ews_stall_results}
    local FORCE_REPS=${FORCE_REPS:-20} R4_REPS=${R4_REPS:-8} W_REPS=${W_REPS:-5} E_REPS=${E_REPS:-10}
    local MAX_ROUNDS=${MAX_ROUNDS:-60} LAST_START=${LAST_START:-$(date -d '2026-10-27 14:10' +%s)}
    local QUIET_LOAD=${QUIET_LOAD:-4} QUIET_DEADLINE=${QUIET_DEADLINE:-$(date -d '2026-10-27 12:00' +%s)}
    local QUEUE_LOG=${QUEUE_LOG:-/home/semih/rig_results_safe/ews_queue.log}
    mkdir -p "$OUT" || return 1
    local t
    for t in $ARMS_ALL; do
        [ -f "$TREES_DIR/$t/PROVENANCE.txt" ] || { echo "REFUSED: $TREES_DIR/$t not prepared"; return 2; }
        cat "$TREES_DIR/$t/PROVENANCE.txt"
    done
    status() { echo "$(date '+%H:%M') ess: $*" >>"$OUT/STATUS.txt"; echo "$(date -Is) $*"; }
    # ---- phase 0: a quiet PC
    status "run $TAG: waiting for a quiet PC ('EWS QUEUE DONE' in $QUEUE_LOG, load1 < $QUIET_LOAD) until" \
        "$(date -d "@$QUIET_DEADLINE" '+%H:%M')"
    local load1
    while :; do
        load1=$(cut -d' ' -f1 /proc/loadavg)
        if grep -q "EWS QUEUE DONE" "$QUEUE_LOG" 2>/dev/null && awk -v l="$load1" -v q="$QUIET_LOAD" 'BEGIN { exit !(l < q) }'; then
            break
        fi
        if [ "$(date +%s)" -gt "$QUIET_DEADLINE" ]; then
            status "NOT QUIET by the deadline (queue done: $(grep -c 'EWS QUEUE DONE' "$QUEUE_LOG" 2>/dev/null)," \
                "load1 $load1): nothing run"
            return 4
        fi
        sleep 60
    done
    status "run $TAG start: quiet (load $(cut -d' ' -f1-3 /proc/loadavg)); FORCE_REPS=$FORCE_REPS R4_REPS=$R4_REPS" \
        "W_REPS=$W_REPS E_REPS=$E_REPS LAST_START=$(date -d "@$LAST_START" '+%F %H:%M')"
    export NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1
    eth_one() { # label tree reps out-subdir
        env OUT="$OUT/${TAG}_$4_$1" REPS="$3" DUR=30 PROFILE=ethernet ARMS="ge0.1b1" CELLS="rel_large" LARGE_HZ=30 \
            BANDS=1 NETEM_BASE="limit 100000 delay 100us rate 1gbit" \
            "$TREES_DIR/$2/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >"$OUT/${TAG}_$4_$1.log" 2>&1
    }
    # ---- phase 1: forced (X, P, Q) and R4's cells, at once
    status "phase 1 (forced X/P/Q x M A S, $FORCE_REPS reps each; P0 R4 $R4_REPS reps a tree) start"
    local pids=() k
    for t in M A S; do
        for k in x p q; do
            eth_one "$t" "${k^^}$t" "$FORCE_REPS" "$k" &
            pids+=($!)
        done
    done
    TREES="M=$TREES_DIR/M A=$TREES_DIR/A S=$TREES_DIR/S" TAG="${TAG}_r4" BASE_OUT="$OUT" \
        PROFILE=wifi REPS="$R4_REPS" ARMS="ctl ge10b4" CELLS="rel_small rel_large" "$HERE/wifi_repair_ab.sh" \
        >/dev/null 2>&1 &
    pids+=($!)
    local p
    for p in "${pids[@]}"; do wait "$p"; done
    status "phase 1 done (load $(cut -d' ' -f1-3 /proc/loadavg))"
    # ---- phase 2: rounds
    eth_rounds() {
        local r=0 pids=() t
        while [ "$r" -lt "$MAX_ROUNDS" ] && [ "$(date +%s)" -le "$LAST_START" ]; do
            r=$((r + 1))
            echo "E round $r start $(date -Is) load $(cut -d' ' -f1 /proc/loadavg)"
            pids=()
            for t in M A S; do
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
            TREES="M=$TREES_DIR/M A=$TREES_DIR/A S=$TREES_DIR/S" TAG="${TAG}_w$r" BASE_OUT="$OUT" \
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
    python3 -I "$HERE/eth_storm_stall_read.py" "$OUT" "$TAG" >"$OUT/$TAG.read.txt" 2>&1
    status "read rc=$? -> $OUT/$TAG.read.txt"
    grep -E '^(CONTROL|LOOP|STALL|NATURAL|NO WORSE|R4|LAND|VOID)' "$OUT/$TAG.read.txt"
}

T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== eth_storm_stall_ab ended mode=$MODE rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
echo "=== eth_storm_stall_ab $MODE $(date -Is) from $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null) load" \
    "$(cut -d' ' -f1-3 /proc/loadavg) ==="
case $MODE in
prepare) prepare ;;
run) run ;;
*) echo "unknown mode $MODE" && false ;;
esac
