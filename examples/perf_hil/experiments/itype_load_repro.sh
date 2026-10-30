#!/usr/bin/env bash
# itype control under PC load (2026-10-31): does the rmw_cyclonedds_cpp arm of the acceptance case `itype` lose its
# graph reading when the PC is busy, and does reading the graph during the window instead of after it fix that?
#
# CI runs 40495418656, 40714327876 and 40853490814 (jazzy) voided itype: the control printed FAIL(type=0/0 qos=0/0)
# with one side at graph_types=2 and the other at 1. Jazzy's rmw_cyclonedds_cpp raises no event for the mismatch, so
# its verdict rests on graph_types alone (NOT APPLICABLE, allowed, at 2/2; FAIL, a VOID, otherwise). Until the fix the
# reading was taken once, after the 10 s window - after the first process to end had destroyed its endpoint.
#
# This PC has lyrical, whose rmw_cyclonedds_cpp raises INCOMPATIBLE_QOS and so PASSes whatever the graph says. The
# verdict jazzy WOULD give is therefore computed here from the same graph_types fields the case reads ("jazzy="), and
# the lyrical verdicts of both arms are recorded beside it.
#
# Usage: itype_load_repro.sh OUT N_QUIET N_LOAD N_NEG
#   OUT/before/scripts  rmw_tickle/scripts at the parent commit; OUT/after/scripts the fix; OUT/neg/scripts the fix with
#   the itype subscriber on ANOTHER topic (a control that cannot meet its publisher) - prepared by the caller.
# Load: LOAD_N busy loops (default 6) at nice LOAD_NICE (0) pinned to CPUS (0-1), each ended by `timeout`; the cases
# run pinned to the same CPUs in both blocks, so the quiet block differs only in the busy loops. Other sessions'
# processes are never touched. Every round takes /home/semih/rig_jobs/pc_ab.lock on a quiet PC (waiting unlocked).
#
# Reading rules (enforced at the end, not only stated):
#   R0 a run whose control arm has no RESULT line on either side is ERROR, counted apart, never a pass or a fail
#   R1 reproduced:   before/load jazzy=FAIL in at least 3 of N_LOAD (with N_LOAD=20 a true rate of 0.3 misses 3 with
#                    probability < 0.04); fewer -> NOT REPRODUCED, and the fix is unvalidated against the failure
#   R2 fixed:        R1 holds and after/load jazzy=FAIL in 0 of N_LOAD
#   R3 can fail:     neg/load jazzy=FAIL AND lyrical control not PASS in every one of N_NEG; any other reading means
#                    the condition cannot decide and the fix is rejected
#   R4 quiet:        before/quiet and after/quiet failures are reported (expected rare; not a gate)
set -uo pipefail
OUT=$1 N_QUIET=$2 N_LOAD=$3 N_NEG=$4
WS=${WS:-/home/semih/rmw_accept_ws}
CPUS=${CPUS:-0-1} LOAD_N=${LOAD_N:-6} LOAD_NICE=${LOAD_NICE:-0}
LOCK=/home/semih/rig_jobs/pc_ab.lock
DEADLINE=$(($(date +%s) + 4 * 3600))
# shellcheck disable=SC2034 # read by the EXIT trap, which shellcheck cannot see
T0=$(date +%s)
LOG=$OUT/runs.txt
LOADPIDS=()
stop_load() {
    local p
    for p in "${LOADPIDS[@]}"; do kill "$p" 2>/dev/null; done
    LOADPIDS=()
}
trap 'stop_load; echo "=== itype_load_repro ended $(date -Is) after $(($(date +%s) - T0)) s ===" >>"$OUT/repro.log"' EXIT
for v in before after neg; do [ -f "$OUT/$v/scripts/rmw_gap_acceptance.sh" ] || { echo "no $OUT/$v/scripts" >>"$OUT/repro.log"; exit 2; }; done
echo "start $(date -Is) N_QUIET=$N_QUIET N_LOAD=$N_LOAD N_NEG=$N_NEG CPUS=$CPUS LOAD_N=$LOAD_N LOAD_NICE=$LOAD_NICE" >>"$OUT/repro.log"
: >"$LOG"

# quiet: no PC_QUIET marker (another job is measuring) and load1 below 4
quiet() { [ ! -e "$HOME/tmp_build/PC_QUIET" ] && awk '{ exit !($1 < 4) }' /proc/loadavg; }
# acquire: the lock, on a quiet PC - never holding the lock while waiting, so a job that wants it is not kept out
acquire() {
    while :; do
        until quiet; do
            [ "$(date +%s)" -ge "$DEADLINE" ] && { echo "VOID: no quiet PC by the deadline" >>"$OUT/repro.log"; exit 3; }
            sleep 30
        done
        exec 9>>"$LOCK" || exit 3
        flock 9
        quiet && return 0
        flock -u 9
        exec 9>&-
    done
}
release() { flock -u 9; exec 9>&-; }
# run_one VARIANT MODE I: one itype case (both arms), one line in runs.txt
run_one() {
    local v=$1 mode=$2 i=$3 od ctl tk gp gs ep es bp bs jazzy
    od=$OUT/$v-$mode-$i
    OUTDIR=$od taskset -c "$CPUS" "$OUT/$v/scripts/rmw_gap_acceptance.sh" -w "$WS" itype >"$od.log" 2>&1
    ctl=$(grep -h '^itype rmw_cyclonedds_cpp | ' "$od/summary.txt" 2>/dev/null | sed 's/^[^|]*| //')
    tk=$(grep -h '^itype rmw_tickle | ' "$od/summary.txt" 2>/dev/null | sed 's/^[^|]*| //')
    f() { grep -h '^RESULT:' "$1" 2>/dev/null | tail -1 | grep -oE "$2=-?[0-9]+" | cut -d= -f2; }
    gp=$(f "$od/itype_rmw_cyclonedds_cpp/itype_pub.log" graph_types)
    gs=$(f "$od/itype_rmw_cyclonedds_cpp/itype_sub.log" graph_types)
    ep=$(f "$od/itype_rmw_cyclonedds_cpp/itype_pub.log" graph_types_end)
    es=$(f "$od/itype_rmw_cyclonedds_cpp/itype_sub.log" graph_types_end)
    bp=$(f "$od/itype_rmw_cyclonedds_cpp/itype_pub.log" graph_both_at_ms)
    bs=$(f "$od/itype_rmw_cyclonedds_cpp/itype_sub.log" graph_both_at_ms)
    if [ -z "$gp" ] || [ -z "$gs" ]; then jazzy=ERROR
    elif [ "$gp" = 2 ] && [ "$gs" = 2 ]; then jazzy=NA; else jazzy=FAIL; fi
    echo "$v $mode $i jazzy=$jazzy graph=${gp:-?}/${gs:-?} end=${ep:--}/${es:--} both_at_ms=${bp:--}/${bs:--}" \
        "load1=$(cut -d' ' -f1 /proc/loadavg) | control: ${ctl:-<none>} | rmw_tickle: ${tk:-<none>}" >>"$LOG"
}
# block MODE COUNT VARIANTS...: COUNT rounds, the variants interleaved within a round. Each round takes the lock on a
# quiet PC (load1 is read BEFORE our busy loops start) and releases it after, with the busy loops stopped.
block() {
    local mode=$1 n=$2 i v
    shift 2
    for i in $(seq "$n"); do
        acquire
        [ "$i" = 1 ] && echo "$mode block: first round $(date -Is) load $(cut -d' ' -f1-3 /proc/loadavg)" >>"$OUT/repro.log"
        if [ "$mode" = load ]; then
            for _ in $(seq "$LOAD_N"); do
                taskset -c "$CPUS" nice -n "$LOAD_NICE" timeout 600 sh -c 'while :; do :; done' &
                LOADPIDS+=($!)
            done
            sleep 3
        fi
        for v in "$@"; do run_one "$v" "$mode" "$i"; done
        stop_load
        release
    done
}
block quiet "$N_QUIET" before after
block load "$N_LOAD" before after
block load "$N_NEG" neg

# The reading rules, from runs.txt
cnt() { grep -c "^$1 $2 .* jazzy=$3 " "$LOG"; }
{
    echo "=== counts (jazzy-equivalent control verdict) ==="
    for key in "before quiet" "after quiet" "before load" "after load" "neg load"; do
        # shellcheck disable=SC2086 # the key is two words on purpose
        set -- $key
        echo "$1/$2: runs=$(grep -c "^$1 $2 " "$LOG") NA=$(cnt "$1" "$2" NA) FAIL=$(cnt "$1" "$2" FAIL) ERROR=$(cnt "$1" "$2" ERROR)" \
            "lyrical control not PASS=$(grep "^$1 $2 " "$LOG" | grep -vc '| control: PASS')" \
            "rmw_tickle not PASS=$(grep "^$1 $2 " "$LOG" | grep -vc '| rmw_tickle: PASS')"
    done
    bf=$(cnt before load FAIL) af=$(cnt after load FAIL)
    nf=$(grep '^neg load ' "$LOG" | grep ' jazzy=FAIL ' | grep -vc '| control: PASS')
    nn=$(grep -c '^neg load ' "$LOG")
    if [ "$bf" -ge 3 ]; then echo "R1 REPRODUCED: before/load FAIL $bf of $N_LOAD"; else echo "R1 NOT REPRODUCED: before/load FAIL $bf of $N_LOAD"; fi
    if [ "$bf" -ge 3 ] && [ "$af" -eq 0 ] && [ "$(cnt after load ERROR)" -eq 0 ]; then echo "R2 FIXED: after/load FAIL 0 of $N_LOAD"
    else echo "R2 NOT SHOWN: after/load FAIL $af ERROR $(cnt after load ERROR) of $N_LOAD (needs R1)"; fi
    if [ "$nn" -eq "$N_NEG" ] && [ "$nf" -eq "$N_NEG" ]; then echo "R3 CAN FAIL: neg/load FAIL on both readings $nf of $N_NEG"
    else echo "R3 CANNOT DECIDE: neg/load FAIL on both readings $nf of $nn (expected $N_NEG) - fix rejected"; fi
} | tee -a "$OUT/repro.log"
