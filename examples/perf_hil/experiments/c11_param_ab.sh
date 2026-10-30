#!/usr/bin/env bash
# c11_param_ab.sh - C11 (2026-10-30): the reveal re-ask as a parameter (tt_LinkOptions.repair_reveal, docs/DESIGN.md
# "Repair reveal"), PC netns only (wifi_burst_loss_netns.sh through wifi_repair_ab.sh). No rig, no default namespace.
#
# WHY. c11_reveal_ab.sh measured the reveal hard-gated to WIFI's repair_timing and large writers: O (full ACKNACK,
# d80877cf) fixed R4 at ge10b4 (p50 113.0 -> 56.5 ms) at 13-18% more resends and 4-10x duplicates (c11ab.read.txt); O2
# (only the order-proven units) kept the duplicates near main and lost the R4 gain (c11o2.read.txt). The user
# (2026-10-30) asked for one general parameter instead of per-medium, per-size handling. The parameter's modes are OFF
# (the default on every profile), REVEALED (O2) and ALL_MISSING (O), for any sample size. This run checks that the
# parameterised code is what was measured: OFF is main, ALL_MISSING is O; and shows what ALL_MISSING does to small
# samples, which the hard gate never reached.
#
# PRE-REGISTERED (written 2026-10-30 before the first run; c11_param_read.py applies every rule in code):
#   Trees (archives with .tree_sha, TREES_DIR, default ~/c11p_trees): M = main 75495519; A = the same commit again (the
#   A/A control); F = the parameter commit with no TT_REPAIR_REVEAL (OFF: the profile's default); O = the same commit
#   with .node_env TT_REPAIR_REVEAL=all_missing (both benches read it, repair_reveal_from_env(); amended after the run:
#   that code moved from a header into each bench, behaviour unchanged). All four at once
#   (wifi_repair_ab.sh), PROFILE=wifi, NODE_ENV=TT_UDP_OFFLOAD=0, LARGE_RSTATS=1, REPS each (default 6): rel_large at
#   ctl ge5b2 ge5b4 ge10b4 tv, then rel_small at ctl ge5b4 tv (the small node, wifi_link_node, now carries the reveal).
#   VOID: the harness's own VOID rules; a rel_large cell of any tree with fewer than 4 unloaded reps.
#   TREATMENT REPORTED: every F rep's REVEAL: line says repair_reveal=1 (OFF) and every O rep's says 3 (ALL_MISSING);
#     a rep without one, or with another value -> VOID (the arm did not run what it names).
#   T_OFF: F sends no reveal ACKNACK in any rep, large or small (RSTATS acknack_tx reveal=0) -> else BUG.
#   T_ALL: O's reveals at ge10b4, median >= 10 a rep -> APPLIED; else the O rows are VOID.
#   C (control): A vs M, the ge10b4 median per-rep p50 spread S; S above 15% of M -> VOID, as c11_reveal_ab.sh.
#   IDENTITY (OFF == main): per rel_large arm and metric (delivered, p50, p99, resends, duplicates, CPU; the floors of
#     c11_reveal_read.py), a row is OFF-FLOOR when |X - M| exceeds the floor alone. IDENTITY HOLDS when T_OFF holds and
#     F has no more OFF-FLOOR rows than A has plus one; else IDENTITY NOT SHOWN, rows listed. (The exact identity, bytes
#     and times, is tests/wire_identity_repair_reveal.sh's; this is the netns view of it.)
#   REPRODUCE (ALL_MISSING == O): O's R4 at ge10b4 is FIXED by c11_reveal_ab.sh's P1 rule (PASS and a fall from M beyond
#     max(2 S, 5% of M)), and O's ge10b4 median per-rep p50 is within 20% of O's 56.5 ms (c11ab) -> REPRODUCED; FIXED
#     but outside 20% -> FIXED, NOT THE SAME FIGURE; else NOT REPRODUCED. The NO WORSE rows (O vs M) are printed as the
#     cost, expected on resends and duplicates as in c11ab.
#   SMALL (descriptive, ALL_MISSING on small samples): per rel_small arm, O against M - delivered %, median per-rep
#     one-way p99, reveals a rep, writer resends and copies after delivery (R7 at tv). Not a gate: it says what the knob
#     does there.
#
# TIME. Waits for the PC A/B lock (flock LOCK, default /home/semih/rig_jobs/pc_ab.lock), then for no
#   ~/tmp_build/PC_QUIET and load1 < 4 (checked every 60 s; gives up at QUIET_DEADLINE, default now + 3 h, VOID).
#   8 cells x REPS reps x ~50 s, the trees at once: ~40 min at REPS=6. Reading < 1 min.
#
# Usage: [TREES_DIR=~/c11p_trees] [OUTB=~/rig_results_safe/c11] [TAG=c11p] [REPS=6] [LOCK=...] c11_param_ab.sh
#   Ends with "=== c11_param_ab ended" in $OUTB/$TAG.driver.log, whatever happened; the reading is $OUTB/$TAG.read.txt.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREES_DIR=${TREES_DIR:-$HOME/c11p_trees}
OUTB=${OUTB:-$HOME/rig_results_safe/c11}
TAG=${TAG:-c11p}
REPS=${REPS:-6}
LOCK=${LOCK:-/home/semih/rig_jobs/pc_ab.lock}
QUIET_DEADLINE=${QUIET_DEADLINE:-$(($(date +%s) + 3 * 3600))}
mkdir -p "$OUTB" || exit 1
LOG="$OUTB/$TAG.driver.log"
READ="$OUTB/$TAG.read.txt"
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== c11_param_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ===" >>"$LOG"
}
trap on_exit EXIT
log() { echo "$(date '+%F %T') $*" >>"$LOG"; }
log "start: trees $TREES_DIR (M A F O), REPS=$REPS, out $OUTB/$TAG*, lock $LOCK"
for t in M A F O; do
    d=$TREES_DIR/$t
    [ -x "$d/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" ] || { log "VOID: no tree $t"; exit 2; }
    [ -s "$d/.tree_sha" ] || { log "VOID: tree $t ($d) has no .tree_sha"; exit 2; }
    log "tree $t: $d commit $(cat "$d/.tree_sha") node_env '$(cat "$d/.node_env" 2>/dev/null)'"
done

exec 9>>"$LOCK" || { log "VOID: cannot open the lock $LOCK"; exit 3; }
log "waiting for the lock"
flock 9
log "lock taken"
quiet() {
    [ ! -e "$HOME/tmp_build/PC_QUIET" ] || return 1
    awk '{ exit !($1 < 4) }' /proc/loadavg
}
until quiet; do
    if [ "$(date +%s)" -ge "$QUIET_DEADLINE" ]; then
        log "VOID: no quiet PC by the deadline (PC_QUIET/load $(cut -d' ' -f1-3 /proc/loadavg))"
        exit 3
    fi
    sleep 60
done
log "quiet: load $(cut -d' ' -f1-3 /proc/loadavg)"

ab() { # ab CELLS ARMS
    env PROFILE=wifi NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1 \
        TREES="M=$TREES_DIR/M A=$TREES_DIR/A F=$TREES_DIR/F O=$TREES_DIR/O" TAG="$TAG" BASE_OUT="$OUTB" REPS="$REPS" \
        CELLS="$1" ARMS="$2" "$TREES_DIR/O/examples/perf_hil/experiments/wifi_repair_ab.sh" >>"$LOG" 2>&1
}
log "phase AB 1: rel_large ctl ge5b2 ge5b4 ge10b4 tv"
ab rel_large "ctl ge5b2 ge5b4 ge10b4 tv"
log "phase AB 2: rel_small ctl ge5b4 tv (load $(cut -d' ' -f1-3 /proc/loadavg))"
ab rel_small "ctl ge5b4 tv"
log "phase AB done (load $(cut -d' ' -f1-3 /proc/loadavg)); reading"
python3 "$HERE/c11_param_read.py" "$OUTB" "$TAG" >"$READ" 2>&1
log "read rc=$? -> $READ"
