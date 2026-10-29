#!/usr/bin/env bash
# c11_reveal_ab.sh - C11 (2026-10-30): the Wi-Fi profile's R4 (1 MiB RELIABLE one-way p50 at ge10b4) against the
# order-revealed re-ask (O, branch ab/wifi-reveal-acknack), PC netns only (wifi_burst_loss_netns.sh, wifi_repair_ab.sh).
# No rig, no default namespace. Also establishes main's R4 and R7 (tv duplicates) on the same run.
#
# WHY. R4 fails at ge10b4 on main (RLF, 2026-10-27: p50 102.9-108.4 ms against a 74.2-74.5 limit; r4s1, 2026-10-27:
# 111.1 ms over 32 reps), with reverse bytes 0.25% of forward (the harness's reading: a timer value, no wire lift).
# Traced on b434f649 (r4_trace_read.py, 299 samples): of a sample near the p50, 42 ms is the drain the control has too,
# 49 ms the reader's wait before it asks again for a repair that was lost (its retry timer, 37 ms at the p50, and its
# answers to the writer's HEARTBEAT chase, which leave a unit out for the repair-transit window). Counterfactual on the
# same trace (c11 reveal_cf.py): a repair resent AFTER the lost one was recorded at the reader 20 ms (p50) after the
# lost resend, and the re-request reached the writer 24 ms (p50) later than an ACKNACK sent at that moment would have;
# moving every unit's rounds that much earlier puts the per-rep p50s at 57-66 ms against 97-119 as traced.
#
# THE CHANGE (O, d80877cf): on a repair_timing link (WIFI), from a writer of large samples, a repair recorded above the
# watermark sends a full ACKNACK at once (maybe_reveal_lost_repairs()), at most one per least repair transit. On a path
# that keeps order the watermark's repair, sent before it, is lost; the writer's resend order (DESIGN.md section 4)
# decides each named unit, holding what is still on its way. No wire change, no estimate touched, ETHERNET unchanged.
# Unlike K (RESULTS `R4K`, held for the wire change by the user on 2026-10-26) it does not touch the retry estimate.
#
# PRE-REGISTERED (written 2026-10-30 before the first run; the python at the end applies every rule):
#   Trees (copies of commits, ~/c11_trees): M = main 73e7a38c, A = the same commit again (the A/A control), O = d80877cf
#   (M + the change). All three at once (wifi_repair_ab.sh), PROFILE=wifi, NODE_ENV=TT_UDP_OFFLOAD=0, LARGE_RSTATS=1:
#   rel_large at ctl ge5b2 ge5b4 ge10b4 tv, then rel_small at ctl ge5b4 tv (rel_small is wifi_link_node, built without
#   tt_LARGE_SAMPLES: the change is compiled out there - a second control), REPS each (default 6).
#   SAMPLE SIZE. 150 samples a rel_large rep, 900 a cell a tree. R4 reads the median of per-rep p50s: main's per-rep p50
#     at ge10b4 spread 86-138 ms (K5, RLF), so 6 reps put the median within ~+-8 ms; the counterfactual's fall is ~40.
#     R7's copies are a count of ~75 a rel_small tv rep: 6 reps ~450, a Poisson SE of ~5%, against a 2.7x excess.
#     NO WORSE at 6 reps resolves only differences above the A/A spread: this is a screen, not a landing census (below).
#   VOID: the harness's own VOID rules (a missing RESULT, a control that lost anything, an arm whose realized loss is
#     off, a rep under 100 samples, load above nproc/2); a rel_large cell of any tree with fewer than 4 unloaded reps.
#   T (treatment): O's reveal ACKNACKs (echo RSTATS acknack_tx reveal=) median >= 10 a rep at ge10b4 -> applied; 0 at ctl
#     in every rep (nothing to reveal without loss; else a BUG line). Not applied -> the run is VOID for P1.
#   C (control): A vs M. The A/A spread of the ge10b4 R4 figure, S = |A - M| of the median per-rep p50. S above 15% of M
#     -> VOID (the instrument cannot resolve the fall). The NO WORSE rule run on A vs M lists the noise rows.
#   P1 (target): O's own R4 line at ge10b4 (wifi_burst_read.py) is PASS, and O's median per-rep p50 at ge10b4 is below
#     M's by more than max(2 S, 5% of M) -> R4 FIXED. PASS with a smaller fall -> UNDECIDED. FAIL -> NOT FIXED, by how
#     many ms over the limit.
#   P2: O's R4 at ge5b4 stays PASS (WORSE if it FAILs where M PASSes).
#   NO WORSE (goodput first): per rel_large arm, metric and tree, O vs M: delivered % (pooled), median per-rep p50 and
#     p99, median rep resends and duplicates, median rep CPU (sub + pub). band = max(|A - M|, floor); floors: delivered
#     0.5 point, p50 5%, p99 10%, resends 10%, duplicates 25% (at least 5), CPU 5%. WORSE when O - M > band (delivered:
#     M - O > band). STORM reps (resends > 10 x M's median at that arm): O more than M -> WORSE. rel_small rows (compiled
#     out) are printed as the noise floor and never count.
#   REVERSE BYTES: O's rev/fwd % at ge10b4 from the harness table, printed; above 2% the harness's reading would call the
#     ACKNACK material (the range-NACK lift) - flagged, and it blocks a landing recommendation.
#   FALSIFIER: T applied and O's ge10b4 fall <= max(2 S, 5% of M) -> FALSIFIED: an order-revealed re-ask is not what
#     holds R4 here; the excess is elsewhere (the trace phase says where).
#   P5 (trace, descriptive, LARGE_TRACE=1 on M and O + r4_trace.patch, ctl + ge10b4 rel_large, 2 reps each, run first):
#     expected - O's mean reask_wait near the p50 at most half of M's, and most critical re-requests carried by 'R'
#     (the reveal) rather than 'T' (timer) or '?' (chase answers). Printed, not a gate.
#   R7 and main's state: every tree's R1-R7 lines are printed; main's R4 and R7 are the "today's state" answer.
#   P6 (R7's mechanism, descriptive; MD = main + r4_trace.patch + r7_diag.patch, tv rel_small, 3 reps, in phase T):
#     r7_diag_read.py classes every copy that reached the reader below its watermark - ABANDONED (given up on an
#     eviction HEARTBEAT before it came: a repair too late), AFTER_RECOVERY (a second answer to the same sample) or
#     OTHER - by tv phase, with the requests the reader sent before each copied sample arrived and its timer interval.
#     Read as: AFTER_RECOVERY with >= 2 requests, mostly the timer's, in phase 2 and the request-to-recovery time above
#     the timer interval -> the reader's retry estimate lags the delay step (a timer value). ABANDONED dominant -> the
#     copies are repairs that came after KEEP_LAST 10 evicted the sample (100 ms of history against a ~50 ms round trip
#     and bursts of 8): a history-depth limit, which no retry timing removes.
#   LAND (a recommendation for the lead, never automatic): T applied, C PASS, R4 FIXED, P2 PASS, NO WORSE 0 rows,
#     reverse bytes <= 2% -> "screen passed: run the landing census" (40 reps an arm, the RLF shape, and the rig core
#     gate v2 + Array1m/4m for cost: the change is compiled into every large build). Otherwise "do not land".
#
# TIME. Quiet wait: until "C7SL QUEUE DONE" in ~/rig_results_safe/c7sl_queue.log, no ~/tmp_build/PC_QUIET, load1 < 4
#   (checked every 120 s; gives up at QUIET_DEADLINE, default 06:30 local, VOID). Trace ~4 min; A/B: 8 cells x REPS reps
#   x ~50 s, the trees at once: ~40 min at REPS=6. Reading < 1 min.
#
# AMENDED 2026-10-30 04:1x, AFTER the first run (c11ab: O FIXED R4, NO WORSE 8 rows on resends/duplicates) and BEFORE
# the second; no rule or threshold moved. The second run (TAG=c11o2) puts O2 (d92618cb: the reveal names only the units
# the arrival shows lost) in the O slot (O_TREE), the same M and A trees, SKIP_TRACE=1; the quiet gate as before (the
# C7SL marker is there; load1 < 4 and no PC_QUIET still checked). Each tree's commit is read from its .tree_sha (the trees are
# archives with no .git, so wifi_repair_ab.sh logs empty SHAs) and logged; a tree without one is VOID.
#
# Usage: [TREES_DIR=~/c11_trees] [O_TREE=$TREES_DIR/O] [OUTB=~/rig_results_safe/c11] [TAG=c11ab] [REPS=6] [SKIP_TRACE=0] c11_reveal_ab.sh
#   Ends with "=== c11_reveal_ab ended" in $OUTB/$TAG.driver.log, whatever happened; the reading is $OUTB/$TAG.read.txt.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREES_DIR=${TREES_DIR:-$HOME/c11_trees}
OUTB=${OUTB:-$HOME/rig_results_safe/c11}
TAG=${TAG:-c11ab}
REPS=${REPS:-6}
O_TREE=${O_TREE:-$TREES_DIR/O}
MARKER_FILE=${MARKER_FILE:-$HOME/rig_results_safe/c7sl_queue.log}
QUIET_DEADLINE=${QUIET_DEADLINE:-$(date -d '06:30' +%s)}
mkdir -p "$OUTB" || exit 1
LOG="$OUTB/$TAG.driver.log"
READ="$OUTB/$TAG.read.txt"
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== c11_reveal_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ===" >>"$LOG"
}
trap on_exit EXIT
log() { echo "$(date '+%F %T') $*" >>"$LOG"; }
log "start: trees $TREES_DIR (M A O MT OT MD), REPS=$REPS, out $OUTB/$TAG*"
for t in M A O MT OT MD; do
    d=$TREES_DIR/$t
    [ "$t" = O ] && d=$O_TREE
    [ "${SKIP_TRACE:-0}" = 1 ] && case $t in MT | OT | MD) continue ;; esac
    [ -x "$d/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" ] || { log "VOID: no tree $t"; exit 2; }
    [ -s "$d/.tree_sha" ] || { log "VOID: tree $t ($d) has no .tree_sha"; exit 2; }
    log "tree $t: $d commit $(cat "$d/.tree_sha")"
done

quiet() {
    grep -q "C7SL QUEUE DONE" "$MARKER_FILE" 2>/dev/null || return 1
    [ ! -e "$HOME/tmp_build/PC_QUIET" ] || return 1
    awk '{ exit !($1 < 4) }' /proc/loadavg
}
until quiet; do
    if [ "$(date +%s)" -ge "$QUIET_DEADLINE" ]; then
        log "VOID: no quiet PC by the deadline (marker/PC_QUIET/load $(cut -d' ' -f1-3 /proc/loadavg))"
        exit 3
    fi
    sleep 120
done
log "quiet: load $(cut -d' ' -f1-3 /proc/loadavg)"

common_env=(PROFILE=wifi NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1)
if [ "${SKIP_TRACE:-0}" != 1 ]; then
    log "phase T (trace): MT and OT at once, ctl + ge10b4 rel_large, 2 reps; MD (R7 diagnostic) tv rel_small, 3 reps"
    pids=()
    for t in MT OT; do
        env "${common_env[@]}" LARGE_TRACE=1 REPS=2 ARMS="ctl ge10b4" CELLS=rel_large OUT="$OUTB/${TAG}_trace_$t" \
            "$TREES_DIR/$t/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >"$OUTB/${TAG}_trace_$t.log" 2>&1 &
        pids+=($!)
    done
    # R7's mechanism (descriptive): main + r4_trace.patch + r7_diag.patch, the tv arm's small cell traced.
    env "${common_env[@]}" SMALL_TRACE=1 REPS=3 ARMS="tv" CELLS=rel_small OUT="$OUTB/${TAG}_r7diag" \
        "$TREES_DIR/MD/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >"$OUTB/${TAG}_r7diag.log" 2>&1 &
    pids+=($!)
    for p in "${pids[@]}"; do wait "$p"; done
    python3 "$HERE/r7_diag_read.py" "$OUTB/${TAG}_r7diag"/runs/tv-rel_small-* >"$OUTB/$TAG.r7diag.txt" 2>&1
    for t in MT OT; do
        {
            echo "--- P5 trace $t (r4_trace_read.py)"
            python3 "$HERE/r4_trace_read.py" "$OUTB/${TAG}_trace_$t"/runs/ge10b4-rel_large-*
        } >>"$OUTB/$TAG.trace.txt" 2>&1
    done
    log "phase T done (load $(cut -d' ' -f1-3 /proc/loadavg))"
fi

ab() { # ab CELLS ARMS
    env "${common_env[@]}" TREES="M=$TREES_DIR/M A=$TREES_DIR/A O=$O_TREE" TAG=$TAG BASE_OUT=$OUTB REPS=$REPS \
        CELLS="$1" ARMS="$2" "$O_TREE/examples/perf_hil/experiments/wifi_repair_ab.sh" >>"$LOG" 2>&1
}
log "phase AB 1: rel_large ctl ge5b2 ge5b4 ge10b4 tv"
ab rel_large "ctl ge5b2 ge5b4 ge10b4 tv"
log "phase AB 2: rel_small ctl ge5b4 tv (load $(cut -d' ' -f1-3 /proc/loadavg))"
ab rel_small "ctl ge5b4 tv"
log "phase AB done (load $(cut -d' ' -f1-3 /proc/loadavg)); reading"
python3 "$HERE/c11_reveal_read.py" "$OUTB" "$TAG" >"$READ" 2>&1
log "read rc=$? -> $READ"
