#!/usr/bin/env bash
# eth_storm_p99_ab.sh - does the progress-held repair timeout (G) cost p99 at 1% loss? A PC netns add-on to
# eth_storm_stall_ab.sh (RESULTS `EWF`), asked by the lead after the rig's Array1m 1% check of G (median p99 +0.54 ms
# on head-built arms: head 10.53, aa 10.48 against pre 9.99 ms). Written and committed before its first run; the
# reading is eth_storm_p99_read.py (its docstring is the rule set, applied in its code).
#
# THE CELL IS NOT THE RIG'S: the rig cell is cross-host Array1m 1% at 30 Hz between two Pis; this is the PC netns
#   ETHERNET profile (1 MiB RELIABLE at 30 Hz, 1 Gbit/s netem, BANDS=1) at 1% random loss (ge1b1). A null here does
#   not clear the rig's +0.54 ms; a shift here says the cost is G's own and not the rig's.
# TREES, all four at once so they share the PC's load rep by rep, from git objects: M = main before G (M_SHA,
#   b61e2302), A = M again (the A/A control), G = G_SHA (45129b71), S = S_SHA (178199cb, G + the stall back-off).
# SAMPLE SIZE (worked out before the run): EWF's 0.1% cell gave a per-rep p99 IQR of 0.66-0.68 ms (robust sigma
#   ~0.5 ms, 1,874 reps at load <= 8). The median's standard error is ~1.25 sigma / sqrt(n), so a difference of two
#   medians has ~1.25 sigma sqrt(2 / n); to see 0.5 ms at two-sided 5% with 80% power needs 0.5 >= 2.8 x that: n >= 24
#   at sigma 0.5, n >= 54 at sigma 0.75. 1% is likely noisier than 0.1%, so REPS = 40 a tree (sigma up to ~0.65),
#   ~30 min. The reading also prints the bootstrap interval, so a wider spread shows as an UNDECIDED, not a null.
# Usage: eth_storm_p99_ab.sh prepare   (M_SHA G_SHA S_SHA; TREES_DIR, default ~/ews_p99_trees; refuses existing)
#        eth_storm_p99_ab.sh run       ([TAG=p99a] [OUT=~/ews_stall_results] [REPS=40] [QUIET_LOAD=4] [TREES_DIR])
#   run waits (at most 30 min) for load1 < QUIET_LOAD, then runs; its log ends with "=== eth_storm_p99_ab ended";
#   the reading is $OUT/$TAG.read.txt; each phase appends a line to $OUT/STATUS.txt.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
TREES_DIR=${TREES_DIR:-$HOME/ews_p99_trees}
MODE=${1:?prepare or run}

prepare() {
    local m=${M_SHA:?M_SHA=main before G} g=${G_SHA:?G_SHA} s=${S_SHA:?S_SHA} t sha
    for t in M A G S; do
        [ -e "$TREES_DIR/$t" ] && { echo "REFUSED: $TREES_DIR/$t exists"; return 2; }
    done
    for t in M A G S; do
        case $t in
        M | A) sha=$m ;;
        G) sha=$g ;;
        S) sha=$s ;;
        esac
        mkdir -p "$TREES_DIR/$t" || return 1
        git -C "$REPO" archive "$sha" | tar -x -C "$TREES_DIR/$t" || return 1
        echo "tree $t: $(git -C "$REPO" rev-parse --short "$sha") src/tickle.c sha256" \
            "$(sha256sum "$TREES_DIR/$t/src/tickle.c" | cut -c1-16)" >"$TREES_DIR/$t/PROVENANCE.txt"
        cat "$TREES_DIR/$t/PROVENANCE.txt"
    done
    cmp -s "$TREES_DIR/M/src/tickle.c" "$TREES_DIR/A/src/tickle.c" || { echo "REFUSED: M and A differ"; return 3; }
    echo "prepare done"
}

run() {
    local TAG=${TAG:-p99a} OUT=${OUT:-$HOME/ews_stall_results} REPS=${REPS:-40} QUIET_LOAD=${QUIET_LOAD:-4} t
    for t in M A G S; do
        [ -f "$TREES_DIR/$t/PROVENANCE.txt" ] || { echo "REFUSED: $TREES_DIR/$t not prepared"; return 2; }
    done
    status() { echo "$(date '+%H:%M') p99: $*" >>"$OUT/STATUS.txt"; echo "$(date -Is) $*"; }
    local deadline=$(($(date +%s) + 1800))
    until awk -v l="$(cut -d' ' -f1 /proc/loadavg)" -v q="$QUIET_LOAD" 'BEGIN { exit !(l < q) }'; do
        if [ "$(date +%s)" -gt "$deadline" ]; then
            status "NOT QUIET within 30 min (load $(cut -d' ' -f1-3 /proc/loadavg)): nothing run"
            return 4
        fi
        sleep 30
    done
    status "run $TAG start (load $(cut -d' ' -f1-3 /proc/loadavg)), REPS=$REPS, cell ETHERNET ge1b1 rel_large"
    local pids=()
    for t in M A G S; do
        env OUT="$OUT/${TAG}_$t" REPS="$REPS" DUR=30 PROFILE=ethernet ARMS="ge1b1" CELLS="rel_large" LARGE_HZ=30 \
            BANDS=1 NETEM_BASE="limit 100000 delay 100us rate 1gbit" NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1 \
            "$TREES_DIR/$t/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >"$OUT/${TAG}_$t.log" 2>&1 &
        pids+=($!)
    done
    local p
    for p in "${pids[@]}"; do wait "$p"; done
    status "reps done (load $(cut -d' ' -f1-3 /proc/loadavg))"
    python3 -I "$HERE/eth_storm_p99_read.py" "$OUT" "$TAG" >"$OUT/$TAG.read.txt" 2>&1
    status "read rc=$? -> $OUT/$TAG.read.txt"
    grep -E '^(CONTROL|P99|STORM|VOID)' "$OUT/$TAG.read.txt"
}

T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== eth_storm_p99_ab ended mode=$MODE rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
echo "=== eth_storm_p99_ab $MODE $(date -Is) load $(cut -d' ' -f1-3 /proc/loadavg) ==="
case $MODE in
prepare) prepare ;;
run) run ;;
*) echo "unknown mode $MODE" && false ;;
esac
