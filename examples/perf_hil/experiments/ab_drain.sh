#!/usr/bin/env bash
# ab_drain.sh - the drain A/B and the rmw confirmation runs, under ONE rig lock, 2026-10-07.
#
# What is measured (arms, all pushed commits fixed to full SHAs at launch):
#   A  main 912eae0e: the block-wait stall fix (cba66e30) and the ping's late-reply fix (912eae0e)
#   B  ab/drain-ring-turn + its witness (5b5b3e07 = 56565720 "drain_rx() ends a pass when the own ring holds a record"
#      + rx_drain_ring_turns): a socket that never empties cannot starve the ring
#   C  REQUIRED, no default: the replacement for 132cd136 ("skip to newest": decode only the newest KEEP_LAST depth
#      samples of a ring backlog, count the rest as rx_shm_skipped_superseded), on top of B. It must contain B's
#      witness commit, or its native treatment check reads NO VERDICT.
#
# Steps, in this order (the two rmw steps on 10.1.1.213 back to back so their bracket is short; s6 is on 10.1.1.214):
#   1. rmw_samehost.sh at A, rmw_tickle only (vendor figures do not depend on our SHA; 7e6fe171's stand), every cell,
#      RMW_REPS reps. The confirmation re-run of the stall fix: block cells must no longer VOID, STALE 0.
#   3. rmw_samehost.sh at C, then at A again (A2), best_effort cells only: delivered/s and the overload latency of C
#      against A1 (step 1's best_effort cells) and A2 - the same build twice is the drift control.
#   2. ab_samehost.sh A/B/C, blocks A B C C B A, S6_REPS reps x S6_DUR s per block and cell, COMPARE="B-A,C-B".
#
# HOW TO READ IT - written before the run and enforced in code (ab_drain.py for steps 1 and 3, ab_samehost.py for 2;
# both pre-registrations are written to $OUTB.prereg.txt / $OUTB.s6.prereg.{json,txt} before the lock and their
# sha256 logged under it):
#   STEP 1   CONFIRMED iff no rmw_tickle block-wait rtt run is VOID and every rtt run's ping printed
#            "STALE: replies_after_deadline=0"; a missing STALE line is NO VERDICT, never "0".
#   STEP 2   PRIMARY (decides, both comparisons, Bonferroni over primaries x 2 comparisons, as ab_compare.py):
#              best_effort_throughput p1/p3/p4: server.win_recv_mbps, server.cpu_s_per_Msample, client.cpu_s_per_Msample
#              reliable_throughput p3: client.win_send_mbps, server.cpu_s_per_Msample
#              reliable_latency p2 and p4 (-i 0.005): client.rtt_p50_ms, client.cpu_s_per_Msample
#            B-A must not be WORSE on any (PASS or IMPROVED): the ring-turn rule may not cost a native cell.
#            C-B must not be WORSE on any: skip-to-newest's gain is at the rmw KEEP_LAST queue (step 3), so natively
#            it may only cost, and the same-host latency p50 and best-effort throughput / cpu per sample are where.
#            CONTROL (moves either way -> VOID): the s6 OFF arm of the same repetitions, segment compiled out
#            (-Dtt_SEGMENT_ENABLED=0): segment_has_records() is constant false there, so B's rule and its counter are
#            dead code, and there is no ring for C to skip in. It cannot be touched by either change, and it runs in
#            the same blocks, minutes apart, which a cross-host cell on other Pis (campaign cell 15/17) could not:
#              best_effort_throughput p3 client.win_send_mbps@OFF, server.cpu_s_per_Msample@OFF;
#              reliable_latency p2 client.rtt_p50_ms@OFF.
#            TREATMENT, every recorded ON repetition: arms B and C client_traffic + server_traffic rx_drain_ring_turns
#            > 0 (shown on the PC: > 0 in all six cells, e.g. p4 best-effort server 12, latency ~1 per round trip);
#            arm A: the traffic line present and no such field. C's own counter is printed per native cell (expected
#            0: the native bench has no KEEP_LAST queue) and decides nothing there.
#   STEP 3   C against A1+A2 pooled (ab_drain.py's docstring): delivered/s of Array1k and Array4k best_effort must be
#            IMPROVED at Bonferroni z, perf_test's latency of those cells HELD within plain 2 x SE of A (the
#            coordinator's rule; 132cd136 broke it: 0.003 -> 1.4 ms on the PC), every best_effort rtt p50 not WORSE.
#            CONTROL A2-A1 beyond 2 x SE VOIDs that metric. TREATMENT: TREATMENT_C_COUNTER (default
#            rx_shm_skipped_superseded>0) summed over the subscriber's sub.log in EVERY C tput run (absent -> NO
#            VERDICT, fails -> VOID), and false in every A run.
#   The changes go to main only if B-A is PASS/IMPROVED, C-B is PASS/IMPROVED and step 3 is IMPROVED.
#
# REPS, from reps_needed.py's formula (n = 2 (z + 0.84)^2 CV^2 / delta^2) over the within-file CV of every earlier
# ab_samehost run on these cells (p1..p4 best-effort, reliable p3, latency p2/p3; 2026-10-06/07): the largest CV is
# 0.92% (reliable_latency p2 rtt_p50 ON), so at the Bonferroni z for 15 x 2 primaries (3.17) S6_REPS=3 per block
# (6 per arm) resolves a 2.1% change at 80% power on every primary, and 1.4% on most. RMW_REPS=3 per phase (6 for
# the pooled A): rmw_tickle's best-effort delivered/s varied ~6% rep to rep at 7e6fe171, and the effect sought is
# several-fold (PC: 6x), while the latency rule is plain 2 x SE.
#
# Before the lock (on this PC, PREFLIGHT=0 skips): rmw_samehost.sh PREFLIGHT_ONLY=1 (its harness, rmw_tickle arm, in a
# private netns), rig_preflight.sh PREFLIGHT_TOPO=samens of A, B and C on the six s6 cells, and the treatment check
# on those preflight runs (ab_drain.py pfwitness). Any failure refuses the rig. PC_ONLY=1 stops there (no lock, and
# unpushed commits are allowed) - the dry run of everything that can run off the rig.
#
# Usage (launch detached; results in ~/rig_results_safe/ab_drain_<stamp>.*, driver log $OUTB.driver.log, which ends
# with "=== ALL_DONE ... rc=N" however the job ends):
#   C=<sha> setsid nohup examples/perf_hil/experiments/ab_drain.sh > ~/rig_results_safe/ab_drain_launch.log 2>&1 < /dev/null &
#   env: A B C S6_REPS(3) S6_DUR(10) RMW_REPS(3) TREATMENT_C_COUNTER PREFLIGHT(1) PC_ONLY(0) RIG_LOCK_WAIT(21600)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
A=${A:-912eae0ed8e93d9c444ef1af38bb60190b24a43f}
B=${B:-5b5b3e0781b3bd195d847c2d2b2bc28c8547d92f}
C=${C:?C=<sha> - the skip-to-newest commit on top of B (no default)}
S6_REPS=${S6_REPS:-3}
S6_DUR=${S6_DUR:-10}
RMW_REPS=${RMW_REPS:-3}
TREATMENT_C_COUNTER=${TREATMENT_C_COUNTER:-rx_shm_skipped_superseded>0}
export RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-21600}

S6_CELLS="best_effort_throughput:p1:;best_effort_throughput:p3:;best_effort_throughput:p4:;reliable_throughput:p3:;\
reliable_latency:p2:-i 0.005;reliable_latency:p4:-i 0.005"
S6_PRIMARY=""
for z in p1 p3 p4; do
    S6_PRIMARY+="best_effort_throughput:$z:server.win_recv_mbps,best_effort_throughput:$z:server.cpu_s_per_Msample,"
    S6_PRIMARY+="best_effort_throughput:$z:client.cpu_s_per_Msample,"
done
S6_PRIMARY+="reliable_throughput:p3:client.win_send_mbps,reliable_throughput:p3:server.cpu_s_per_Msample,"
for z in p2 p4; do
    S6_PRIMARY+="reliable_latency:$z:-i 0.005:client.rtt_p50_ms,reliable_latency:$z:-i 0.005:client.cpu_s_per_Msample,"
done
S6_PRIMARY=${S6_PRIMARY%,}
S6_CONTROL="best_effort_throughput:p3:client.win_send_mbps@OFF,best_effort_throughput:p3:server.cpu_s_per_Msample@OFF,\
reliable_latency:p2:-i 0.005:client.rtt_p50_ms@OFF"
S6_TREATMENT="*:B,C:client_traffic.rx_drain_ring_turns+server_traffic.rx_drain_ring_turns>0;\
*:A:client_traffic.tx_datagrams>=0 and absent_as(client_traffic.rx_drain_ring_turns,0)+absent_as(server_traffic.rx_drain_ring_turns,0)==0"

s6_env() { # the environment ab_samehost.py's pre-registration is built from - identical before and under the lock
    env TAG=ab_drain A="$A" B="$B" C="$C" CELLS="$S6_CELLS" PRIMARY="$S6_PRIMARY" CONTROL="$S6_CONTROL" \
        TREATMENT="$S6_TREATMENT" SECONDARY="" DERIVED="" COMPARE="B-A,C-B" REPS="$S6_REPS" DUR="$S6_DUR" "$@"
}

ROLE=outer
[ "${AB_DRAIN_INNER:-0}" = 1 ] && ROLE=inner
LOG=/dev/null
on_exit() { # the one EXIT trap: the outer process always ends the driver log with ALL_DONE, success or not
    local rc=$?
    if [ "$ROLE" = outer ]; then
        local v=""
        [ -n "${OUTB:-}" ] && [ -f "$OUTB.verdicts.txt" ] && v=" | $(tr '\n' ' ' <"$OUTB.verdicts.txt")"
        echo "=== ALL_DONE $(date -Is) rc=$rc${v} ===" | tee -a "$LOG"
    else
        echo "=== ab_drain under the lock ended $(date -Is) rc=$rc ===" >>"$LOG"
    fi
}
trap on_exit EXIT
trap 'exit 143' TERM INT HUP # so a kill still runs the EXIT trap

# ------------------------------------------------------------------------------------------------- outer
if [ "$ROLE" = outer ]; then
    if [ -z "${OUTB:-}" ]; then
        git -C "$REPO" fetch -q origin || echo "warning: git fetch origin failed" >&2
        for v in A B C; do
            full=$(git -C "$REPO" rev-parse --verify -q "${!v}^{commit}") || { echo "REFUSED: $v=${!v} does not resolve" >&2; exit 2; }
            if [ "${PC_ONLY:-0}" != 1 ] && [ -z "$(git -C "$REPO" branch -r --contains "$full" 2>/dev/null)" ]; then
                echo "REFUSED: $v=${!v} ($full) is on no origin branch - push it first (PC_ONLY=1 allows it off the rig)" >&2
                exit 2
            fi
            printf -v "$v" '%s' "$full"
        done
        [ "$A" != "$B" ] && [ "$B" != "$C" ] && [ "$A" != "$C" ] || { echo "REFUSED: two arms are the same commit" >&2; exit 2; }
        git -C "$REPO" merge-base --is-ancestor "$B" "$C" ||
            echo "warning: C=$C does not contain B=$B (its witness): C's native treatment will read NO VERDICT" >&2
        python3 "$X/ab_drain.py" check-counter "$TREATMENT_C_COUNTER" >/dev/null || exit 2
        OUTB=$HOME/rig_results_safe/ab_drain_$(date +%Y%m%d-%H%M%S)
        mkdir -p "$(dirname "$OUTB")" || exit 1
    fi
    export A B C OUTB
    LOG=$OUTB.driver.log
    {
        echo "=== ab_drain $(date -Is) on $(hostname) ==="
        echo "    A=$A"
        echo "    B=$B"
        echo "    C=$C"
        echo "    S6_REPS=$S6_REPS S6_DUR=$S6_DUR RMW_REPS=$RMW_REPS TREATMENT_C_COUNTER=$TREATMENT_C_COUNTER"
    } | tee -a "$LOG"
    python3 "$X/ab_drain.py" prereg >"$OUTB.prereg.txt" || exit 2
    s6_env python3 "$X/ab_samehost.py" prereg "$OUTB.s6.prereg.json" >"$OUTB.s6.prereg.txt" ||
        { cat "$OUTB.s6.prereg.txt"; echo "REFUSED: the native A/B's pre-registration" | tee -a "$LOG"; exit 2; }
    cat "$OUTB.s6.prereg.txt" >>"$LOG"
    # The estimate, from the two harnesses' own costs: s6 ~30 s build/setup per call plus ON and OFF arms of
    # DUR + 16 s per repetition (ab_samehost.sh); rmw ~28 s per run (rmw_samehost 7e6fe171: 153 runs in 70 min),
    # ~3 min to rebuild rmw_tickle/perf_test/ping at a new SHA and ~1.5 min for each dry run.
    s6_s=$((6 * 6 * (30 + S6_REPS * 2 * (S6_DUR + 16))))
    rmw_s=$(((12 + 6 + 6) * RMW_REPS * 28 + 3 * (180 + 90)))
    echo "ESTIMATE: rmw steps 1+3 ~$((rmw_s / 60)) min (10.1.1.213), native A/B ~$((s6_s / 60)) min (10.1.1.214)," \
        "total ~$(((rmw_s + s6_s) / 60)) min on the rig, plus the lock wait" | tee -a "$LOG"
    if [ "${PREFLIGHT:-1}" != 0 ]; then
        # It runs ~/tickle/install's rmw_tickle, not an arm (rmw_samehost.sh's header), and that build may predate B:
        # then tput Array4k best_effort can lose the announce race B fixes and VOID on its instrument cross-check (PC,
        # 2026-10-07: 1 of 3 runs, "tx_shm share 0.977 but bytes witness 0.97"). One retry, both logged; a harness
        # defect fails both.
        for try in 1 2; do
            echo "--- PC preflight $(date +%T): rmw_samehost harness (rmw_tickle arm, private netns), try $try" | tee -a "$LOG"
            ARMS=tickle PREFLIGHT_ONLY=1 PREFLIGHT_OUT="$OUTB.pf_rmw$try" "$X/rmw_samehost.sh" >"$OUTB.pf_rmw$try.log" 2>&1 &&
                break
            grep -A4 '^VOID runs' "$OUTB.pf_rmw$try.log" | tee -a "$LOG"
            if [ "$try" = 2 ]; then
                tail -20 "$OUTB.pf_rmw$try.log"
                echo "REFUSING TO TAKE THE RIG: rmw_samehost PC preflight failed twice ($OUTB.pf_rmw*.log)" | tee -a "$LOG"
                exit 1
            fi
        done
        specs=()
        while IFS='|' read -r scen size extra _; do
            [[ $scen == BLOCKS* ]] && continue
            specs+=("$scen:$size:N0:$extra:-Q")
        done < <(python3 "$X/ab_samehost.py" cells "$OUTB.s6.prereg.json")
        for v in A B C; do
            sha=${!v}
            echo "--- PC preflight $(date +%T): arm $v ${sha:0:8}, ${#specs[@]} s6 cells, samens" | tee -a "$LOG"
            SHA=$sha PREFLIGHT_TOPO=samens FWS=tickle PREFLIGHT_OUT="$OUTB.pf_s6_$v" "$X/rig_preflight.sh" "${specs[@]}" \
                >"$OUTB.pf_s6_$v.log" 2>&1 || {
                tail -20 "$OUTB.pf_s6_$v.log"
                echo "REFUSING TO TAKE THE RIG: preflight of arm $v failed ($OUTB.pf_s6_$v.log)" | tee -a "$LOG"
                exit 1
            }
            want=present
            [ "$v" = A ] && want=absent
            echo "    treatment witness rx_drain_ring_turns, want $want:" | tee -a "$LOG"
            python3 "$X/ab_drain.py" pfwitness "$OUTB.pf_s6_$v" "$want" | tee -a "$LOG"
            [ "${PIPESTATUS[0]}" = 0 ] || {
                echo "REFUSING TO TAKE THE RIG: arm $v does not show its treatment on the PC" | tee -a "$LOG"
                exit 1
            }
        done
    fi
    if [ "${PC_ONLY:-0}" = 1 ]; then
        echo "PC_ONLY=1: pre-registrations written, every PC step passed; no lock taken, nothing run on the rig" | tee -a "$LOG"
        exit 0
    fi
    echo "--- taking the rig lock $(date -Is) (wait up to ${RIG_LOCK_WAIT}s)" | tee -a "$LOG"
    AB_DRAIN_INNER=1 "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
    exit $?
fi

# ------------------------------------------------------------------------------------------------- inner, under the lock
LOG=$OUTB.driver.log
say() { echo "$*" | tee -a "$LOG"; }
verdict() { echo "$*" >>"$OUTB.verdicts.txt"; say "$*"; }
: >"$OUTB.verdicts.txt"
say "=== under the rig lock $(date -Is) ==="
say "    prereg sha256 $(sha256sum "$OUTB.prereg.txt" | cut -c1-64) ($OUTB.prereg.txt)"
say "    s6 prereg sha256 $(sha256sum "$OUTB.s6.prereg.json" | cut -c1-64) ($OUTB.s6.prereg.json)"

rmw() { # rmw <out stem> <sha> [extra env...]: one rmw_samehost.sh run, rmw_tickle only, inside this lock
    local out=$1 sha=$2
    shift 2
    say "--- rmw_samehost ${sha:0:8} -> $out ($(date +%T))"
    env SHA="$sha" OUT="$out" ARMS=tickle REPS="$RMW_REPS" PREFLIGHT=0 "$@" "$X/rmw_samehost.sh" >"$out.log" 2>&1
    say "    exit $?; $(grep -c '^  [0-9:]\{8\} ' "$out.txt" 2>/dev/null || echo 0) runs logged"
}

# Step 1: A, every cell.
rmw "$OUTB.s1_rmw_A1" "$A"
python3 "$X/ab_drain.py" step1 "$OUTB.s1_rmw_A1" >"$OUTB.s1_verdict.txt" 2>&1
cat "$OUTB.s1_verdict.txt" >>"$LOG"
verdict "$(grep -m1 '^STEP1 VERDICT' "$OUTB.s1_verdict.txt" || echo 'STEP1 VERDICT: NO VERDICT - the reader printed none')"

# Step 3: C then A again, best_effort cells.
rmw "$OUTB.s3_rmw_C" "$C" QOSES=best_effort
rmw "$OUTB.s3_rmw_A2" "$A" QOSES=best_effort
python3 "$X/ab_drain.py" step3 "$OUTB.s1_rmw_A1" "$OUTB.s3_rmw_C" "$OUTB.s3_rmw_A2" "$TREATMENT_C_COUNTER" \
    >"$OUTB.s3_verdict.txt" 2>&1
cat "$OUTB.s3_verdict.txt" >>"$LOG"
verdict "$(grep -m1 '^STEP3 VERDICT' "$OUTB.s3_verdict.txt" || echo 'STEP3 VERDICT: NO VERDICT - the reader printed none')"

# Step 2: the native A/B. ab_samehost.sh re-derives its pre-registration from this same environment and refuses if it
# is not byte-identical to $OUTB.s6.prereg.json; RIG_LOCK_HELD_HIL=1 (from rig_lock.sh) skips its own preflight/lock.
say "--- native A/B, ab_samehost.sh ($(date +%T))"
s6_env OUTB="$OUTB.s6" PREFLIGHT=0 "$X/ab_samehost.sh" >"$OUTB.s6.log" 2>&1
say "    exit $?"
say "    C's counter in the native cells (information; 0 expected, no KEEP_LAST queue there):"
python3 "$X/ab_drain.py" s6counter "$OUTB.s6" "${C:0:8}" "$(python3 "$X/ab_drain.py" check-counter "$TREATMENT_C_COUNTER")" |
    tee -a "$LOG"
if [ -f "$OUTB.s6.summary.txt" ]; then
    while read -r l; do verdict "STEP2 $l"; done < <(grep -E '^(VERDICT|OVERALL)' "$OUTB.s6.summary.txt")
else
    verdict "STEP2 OVERALL: NO VERDICT - no $OUTB.s6.summary.txt (see $OUTB.s6.log)"
fi
say "=== ab_drain steps done $(date -Is) ==="
