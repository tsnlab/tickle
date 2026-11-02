#!/usr/bin/env bash
# ethernet_seed_ab.sh - A/B of the ETHERNET retry-estimate seed (2026-11-02), as ONE detached chain:
#   wait for the T1 netns run to end -> build B and run the unit tests the fix touches -> the A/B under the box lock
#   -> ethernet_seed_ab_read.py -> claude-notify -> "ETHSEED_CHAIN_END <status>" (always, from the one EXIT trap).
#
# WHAT IS TESTED. In time_varying_netns.sh's smoke (2026-11-02) the static arm s_d20 (ETHERNET, netem 20 ms each way,
# 2% loss) copied 30% of rel_small's samples after delivery (692/2300) against 0.04% at s_d1. Mechanism (read from the
# code and the run's RSTATS): on a link without repair_timing (every profile but WIFI) end_watermark_probe() times a
# recovery into the reader's estimate only within tt_RELIABLE_RETRY (3) retries; before the first sample the interval
# is retry_initial (1 ms), so on a round trip above ~4 ms no recovery ever qualifies, recovery_srtt_ns stays 0 and the
# timer asks every 1-8 ms for the whole run (the back-off resets each time the 100 Hz stream moves the frontier):
# ~15 timer ACKNACKs per gap, each answered by a resend that lands after the repair. The fix (B) times the first
# recovery whatever its retries, as WIFI's repair_timing_times() has since 2026-10-23.
#
# ARMS. Three trees, each in a worktree of its own made here from its ref, built by each harness from that tree into a
# fresh mktemp directory on every invocation (no binary outlives a run; nothing is restored in place; no colcon):
#   A  = A_REF (ab/ethernet-seed-base: origin/main dc45d3d9 + these experiments-only commits)
#   B  = B_REF (ab/ethernet-seed: A + the fix commit alone - checked, see IDENTITY)
#   A2 = A_REF again, in a second worktree: the A/A that sets every tolerance.
# Order per rep, rotated against drift: rep 1 A B A2, rep 2 B A2 A, rep 3 A2 A B. Per label and rep:
#   tv         time_varying_netns.sh PROFILE=ethernet ARMS="s_d1 s_d3 s_d20 D_p2" (+ s_l0, the control it always adds)
#              CELLS="rel_small rel_large"
#   wbe_small  wifi_burst_loss_netns.sh PROFILE=ethernet ARMS="ctl ge1b2 ge5b2 ge5b4 ge10b4 tv" CELLS=rel_small
#   wbe_large  the same, CELLS=rel_large, NODE_ENV=TT_UDP_OFFLOAD=0 (as WB2)
#   wbw_small, wbw_large   the same two on PROFILE=wifi: the CONTROL - the fix changes only the branch a link without
#              repair_timing takes, so nothing there can move; with ctl and s_l0 (no loss, no recovery) they show the
#              noise the A/A spread must cover.
# DUR=24 s everywhere (2300 scored small samples, 120 large a run), REPS=3.
#
# PRE-REGISTERED (written 2026-11-02 before any A/B run; ethernet_seed_ab_read.py implements each rule and it - not
# this comment - decides; constants at its top):
#   Tolerance of any B-vs-A comparison: max(2 x |A - A2|, 2 x SE of the A and A2 reps, a floor per metric: 0.5 point
#     for delivered and copies, 10% or 1 ms for a p99, 10% + 5 for resends, 0.5 for timer ACKNACKs per gap).
#   P1 (decision, tv s_d20 rel_small): B's copies after delivery (echo reader's late_below_ack + duplicates over its
#     delivered) <= 3% in EVERY rep, and B's timer ACKNACKs per gap opened (both readers pooled) <= 2 -> PASS.
#     B's mean above 10% -> FALSIFIED (the seed is not the mechanism). Otherwise FAIL.
#   P1d (report): s_d3 (3 ms each way) - A's timer ACKNACKs per gap >= 2.5 (predicted ~4) and B's <= 1.5: CONFIRMED,
#     else NOT_CONFIRMED (the mechanism's dose-response; does not change the verdict).
#   P2 (tv s_d20 rel_small): B delivers 100% in every rep and B's round-trip p99 <= A's + 40 ms (a second-round loss now
#     waits about one round trip instead of 1 ms).
#   P3 (regression: tv s_d1, wbe ge1b2 ge5b2 ge5b4 ge10b4 tv; rel_small and rel_large): no metric (delivered, p99,
#     copies, resends) worse than A beyond the tolerance, and no R1 (rel_small >= 99.5% at ge5b2/ge5b4) or R3
#     (rel_large >= 95% at ge5b4/ge10b4) that A meets and B does not.
#   P4 (report): tv D_p2 and wbe tv rel_small copies. Predicted unchanged (the seed happens in their 1 ms phase); B
#     under half of A beyond the tolerance means the cold-start-only reading is wrong.
#   P5 (tv s_d20 rel_large): B's timer ACKNACKs and resends not worse than A's beyond the tolerance.
#   VERDICT: VOID on any VOID below or P1 VOID; FALSIFIED on P1 FALSIFIED; PASS when P1, P2, P3 and P5 pass; MIXED when
#     P1 passes and the rest only fail P5 or cannot be judged; FAIL otherwise.
#   VOID (outcomes, never config):
#     - IDENTITY: B's parent is not A, A2 is not A, A..B changes anything but src/tickle.c and
#       tests/test_reliable_pubsub.c, or an OUT's provenance names another tree or a dirty one. This is the treatment
#       check: that B runs the fix and A does not is a property of the trees, read from git, so that an ineffective fix
#       reads as FAIL/FALSIFIED and not as VOID. (Amended before any run from the first proposal, which voided a B
#       that "looked like A" - the same outcome FALSIFIED reads, so it could not tell the two apart.)
#     - a rep whose clean arm (s_l0, ctl) lost anything (RELIABLE under 100%, any tc drop): that rep of that group;
#     - fewer than 2 usable reps of a label in a judged cell (that rule VOID);
#     - A/A: A and A2 more than 5 points apart on s_d20 rel_small copies;
#     - the phenomenon: A or A2 under 10% copies at s_d20 (the smoke's ~30% not reproduced: nothing exercised);
#     - the control: more than 10% (and more than 1) of the control comparisons outside the tolerance;
#     - a run the harness readers void (missing RESULT, never matched, send deficit, profile treatment) is dropped;
#       a run started at a PC 1-minute load above nproc/2 keeps its delivery and copies and loses its latency.
#
# THE CHAIN, with estimates (the 1.5x rule: each wait carries its deadline):
#   0. preflight: refs resolved, B's parent and diff checked, three worktrees made        < 1 min
#   1. wait for T1: its launch log prints "=== time_varying_netns ended" (T1_LOG, default
#      $(cat ~/rig_jobs/t1_out_path.txt).launch.log). Deadline T1_DEADLINE (default 23:40 today) -> T1_WAIT_TIMEOUT;
#      a log not written for 20 min before that -> T1_SILENT. Either way the chain stops and says so.
#   2. unit tests in B: test_reliable_pubsub and test_large_sample, built into B's worktree      ~ 5 min (timeout 20)
#      (OBJ=obj/ethseed). A failure stops the chain (TESTS_FAILED) after running A's same two as the control.
#   3. the A/B under RIG_LOCK_SCOPE=box (taken inside this process; waits up to 1 h): 9 label-reps x 34 runs x ~31 s
#      + 45 harness builds                                                                      ~ 3 h 20 min
#      Each harness call carries a timeout (tv 25 min, wifi_burst 15 min); one that ends without its "ended" line, or
#      by the timeout, stops the chain (AB_STEP_FAILED) - a VOID-by-hand is not left to the reader to discover.
#   4. the reader                                                                                  < 1 min
#   Total after T1 ends: ~3.5 h.
#
# Usage (detached, outside any session directory):
#   setsid nohup examples/perf_hil/experiments/ethernet_seed_ab.sh > ~/rig_jobs/ethseed_<stamp>.log 2>&1 < /dev/null &
#   Env: A_REF B_REF REPO_GIT (/home/semih/tickle) RES (~/rig_results_safe/ethernet_seed/<stamp>) REPS (3) DUR (24)
#        T1_LOG T1_DEADLINE ("23:40") NOTIFY_SESSION (tickle-27) SKIP_T1_WAIT=1 (no wait) KEEP_WT=1 (keep worktrees)
#        STOP_AFTER=preflight|t1 (stop after the preflight or the T1 wait, through the trap: checks of the chain that
#        run nothing)
#        NOTIFY_DRY=1 (claude-notify --dry-run)
#        ETHSEED_EXPECTED_DIFF (comma-separated, default src/tickle.c,tests/test_reliable_pubsub.c): the paths A..B
#        must change, exactly - for a B that also re-pins other tests (the Karn's-rule B, 2026-11-03)
#   Every line it prints goes to RES/chain.log too; the last is always "ETHSEED_CHAIN_END <status>". To stop it, TERM
#   its process group (setsid made one): kill -TERM -<pgid>. A TERM to the chain alone waits for the step it is in.
set -uo pipefail

# --- the A/B phase, run by the chain under the box lock (rig_lock.sh execs this script with __ab) -------------------
if [ "${1:-}" = __ab ]; then
    say() { echo "$(date '+%F %T') $*" | tee -a "$RES/chain.log"; }
    WB_ARMS="ctl ge1b2 ge5b2 ge5b4 ge10b4 tv"
    order() {
        case "$1" in
        1) echo "A B A2" ;;
        2) echo "B A2 A" ;;
        *) echo "A2 A B" ;;
        esac
    }
    # step LABEL REP GROUP TIMEOUT ENDED_PATTERN EXPECTED_RUNS HARNESS [VAR=VALUE...]: one harness call into
    # RES/LABEL/GROUP. It failed - and the chain stops - when it timed out, when its summary gained no new "ended"
    # line (the summary accumulates over reps, so the count must rise by one), or when not one of this rep's runs left
    # a RESULT line (a build that failed, a topology that never came up). A few runs without one are void runs, which
    # the reader drops and counts; the harness's own reader failing is logged and does not stop the chain.
    step() {
        local label=$1 rep=$2 group=$3 limit=$4 pattern=$5 expected=$6 harness=$7
        shift 7
        local out="$RES/$label/$group" x="$WTROOT/$label/examples/perf_hil/experiments"
        mkdir -p "$out"
        local ended_before
        ended_before=$(grep -c "$pattern" "$out/summary.txt" 2>/dev/null)
        say "STEP start $label rep $rep $group"
        env -u NODE_ENV -u ARMS -u CELLS "$@" OUT="$out" REP_FIRST="$rep" REPS=1 DUR="$DUR" \
            timeout "$limit" "$x/$harness" >>"$RES/$label/$group.log" 2>&1
        local rc=$? ended_after with_result=0 run_dir
        ended_after=$(grep -c "$pattern" "$out/summary.txt" 2>/dev/null)
        for run_dir in "$out"/runs/*-"$rep"; do
            [ -d "$run_dir" ] && grep -q '^RESULT' "$run_dir/ping.log" 2>/dev/null && with_result=$((with_result + 1))
        done
        if [ "$rc" = 124 ] || [ "${ended_after:-0}" -ne $((${ended_before:-0} + 1)) ] || [ "$with_result" = 0 ]; then
            say "STEP FAILED $label rep $rep $group rc=$rc ended ${ended_before:-0}->${ended_after:-0}" \
                "runs with RESULT $with_result/$expected (timeout ${limit}s): see $RES/$label/$group.log"
            exit 3
        fi
        say "STEP done  $label rep $rep $group rc=$rc runs with RESULT $with_result/$expected"
    }
    for rep in $(seq 1 "$REPS"); do
        for label in $(order "$rep"); do
            say "LABEL $label rep $rep load=$(cut -d' ' -f1 /proc/loadavg)"
            step "$label" "$rep" tv 1500 '^=== time_varying_netns ended' 10 time_varying_netns.sh \
                PROFILE=ethernet ARMS="s_d1 s_d3 s_d20 D_p2" CELLS="rel_small rel_large"
            step "$label" "$rep" wbe_small 900 '^=== wifi_burst_loss_netns ended' 6 wifi_burst_loss_netns.sh \
                PROFILE=ethernet ARMS="$WB_ARMS" CELLS=rel_small
            step "$label" "$rep" wbe_large 900 '^=== wifi_burst_loss_netns ended' 6 wifi_burst_loss_netns.sh \
                PROFILE=ethernet ARMS="$WB_ARMS" CELLS=rel_large NODE_ENV=TT_UDP_OFFLOAD=0
            step "$label" "$rep" wbw_small 900 '^=== wifi_burst_loss_netns ended' 6 wifi_burst_loss_netns.sh \
                PROFILE=wifi ARMS="$WB_ARMS" CELLS=rel_small
            step "$label" "$rep" wbw_large 900 '^=== wifi_burst_loss_netns ended' 6 wifi_burst_loss_netns.sh \
                PROFILE=wifi ARMS="$WB_ARMS" CELLS=rel_large NODE_ENV=TT_UDP_OFFLOAD=0
        done
    done
    say "AB_PHASE_COMPLETE"
    exit 0
fi

# --- the chain -------------------------------------------------------------------------------------------------------
REPO_GIT=${REPO_GIT:-/home/semih/tickle}
A_REF=${A_REF:-ab/ethernet-seed-base}
B_REF=${B_REF:-ab/ethernet-seed}
STAMP=$(date +%Y%m%d-%H%M%S)
export RES=${RES:-$HOME/rig_results_safe/ethernet_seed/$STAMP}
export REPS=${REPS:-3}
export DUR=${DUR:-24}
export WTROOT=$HOME/tmp_build/ethseed_wt_$STAMP
SESSION=${NOTIFY_SESSION:-tickle-27}
T1_DEADLINE=${T1_DEADLINE:-23:40}
mkdir -p "$RES" || exit 1
say() { echo "$(date '+%F %T') $*" | tee -a "$RES/chain.log"; }
STATUS=ERROR_EARLY_EXIT
VERDICT=""
MADE_WT=()

# shellcheck disable=SC2329 # invoked by the EXIT trap below
on_exit() {
    local rc=$? wt
    # Killed, the A/B phase - a child under the lock - may still be running from these worktrees: they stay.
    if [ "${KEEP_WT:-0}" != 1 ] && [ "$STATUS" != KILLED ]; then
        for wt in "${MADE_WT[@]}"; do
            git -C "$REPO_GIT" worktree remove --force "$wt" >/dev/null 2>&1 || say "could not remove worktree $wt"
        done
        [ -d "$WTROOT" ] && rmdir "$WTROOT" 2>/dev/null
    fi
    local level=confirm title body
    case "$STATUS" in
    VERDICT_PASS)
        level="done"
        title="이더넷 seed A/B 완료: PASS"
        body="판정 PASS. 결과: $RES/verdict.txt"
        ;;
    VERDICT_*)
        title="이더넷 seed A/B 완료: ${STATUS#VERDICT_}"
        body="판정 ${STATUS#VERDICT_} - 확인 필요. 결과: $RES/verdict.txt"
        ;;
    TESTS_FAILED)
        title="이더넷 seed A/B 중단: 단위 테스트 실패"
        body="B 트리의 단위 테스트가 실패해 A/B를 시작하지 않았습니다. 로그: $RES/tests.log"
        ;;
    T1_WAIT_TIMEOUT | T1_SILENT)
        title="이더넷 seed A/B 시작 안 함: T1 대기 실패"
        body="T1 종료 줄을 기한 안에 보지 못했습니다($STATUS). A/B는 실행되지 않았습니다. 로그: $RES/chain.log"
        ;;
    *)
        title="이더넷 seed A/B 중단: $STATUS"
        body="체인이 중간에 멈췄습니다($STATUS). 로그: $RES/chain.log"
        ;;
    esac
    local dry=()
    [ "${NOTIFY_DRY:-0}" = 1 ] && dry=(--dry-run)
    if claude-notify "${dry[@]}" "$SESSION" "$level" "$title" "$body" >>"$RES/chain.log" 2>&1; then
        say "notify sent ($level)"
    else
        say "notify NOT delivered (claude-notify exit non-zero)"
    fi
    say "ETHSEED_CHAIN_END $STATUS rc=$rc res=$RES"
}
trap on_exit EXIT
trap 'STATUS=KILLED; exit 143' TERM INT HUP

say "=== ethernet_seed_ab chain $(date -Is): A_REF=$A_REF B_REF=$B_REF RES=$RES REPS=$REPS DUR=$DUR pid=$$"

# 0. preflight -------------------------------------------------------------------------------------------------------
STATUS=ERROR_PREFLIGHT
A_SHA=$(git -C "$REPO_GIT" rev-parse --verify "$A_REF^{commit}") || exit 1
B_SHA=$(git -C "$REPO_GIT" rev-parse --verify "$B_REF^{commit}") || exit 1
B_PARENT=$(git -C "$REPO_GIT" rev-parse --verify "$B_SHA^1") || exit 1
DIFF_PATHS=$(git -C "$REPO_GIT" diff --name-only "$A_SHA" "$B_SHA" | sort | paste -sd, -)
for label in A B A2; do
    sha=$A_SHA
    [ "$label" = B ] && sha=$B_SHA
    wt="$WTROOT/$label"
    if ! git -C "$REPO_GIT" worktree add --detach "$wt" "$sha" >>"$RES/chain.log" 2>&1; then
        say "cannot make worktree $wt"
        exit 1
    fi
    MADE_WT+=("$wt")
done
A_SHORT=$(git -C "$WTROOT/A" rev-parse --short HEAD)
B_SHORT=$(git -C "$WTROOT/B" rev-parse --short HEAD)
A2_SHA=$(git -C "$WTROOT/A2" rev-parse HEAD)
echo "a_sha=$A_SHA b_sha=$B_SHA a2_sha=$A2_SHA b_parent=$B_PARENT a_short=$A_SHORT b_short=$B_SHORT" \
    "diff_paths=$DIFF_PATHS a_ref=$A_REF b_ref=$B_REF" >"$RES/identity.txt"
say "identity: $(cat "$RES/identity.txt")"
export ETHSEED_EXPECTED_DIFF=${ETHSEED_EXPECTED_DIFF:-src/tickle.c,tests/test_reliable_pubsub.c} # the reader checks it too
if [ "$B_PARENT" != "$A_SHA" ] || [ "$DIFF_PATHS" != "$(tr , '\n' <<<"$ETHSEED_EXPECTED_DIFF" | sort | paste -sd, -)" ]; then
    say "B is not A + the fix alone (parent $B_PARENT, paths $DIFF_PATHS): not running an A/B that cannot read"
    exit 1
fi

if [ "${STOP_AFTER:-}" = preflight ]; then
    STATUS=STOPPED_AFTER_PREFLIGHT
    exit 0
fi

# 1. wait for T1 -----------------------------------------------------------------------------------------------------
if [ "${SKIP_T1_WAIT:-0}" != 1 ]; then
    STATUS=T1_WAIT
    T1_LOG=${T1_LOG:-$(cat "$HOME/rig_jobs/t1_out_path.txt").launch.log}
    deadline=$(date -d "$T1_DEADLINE" +%s) || exit 1
    say "waiting for '=== time_varying_netns ended' in $T1_LOG until $(date -d "@$deadline" '+%F %T')"
    [ -f "$T1_LOG" ] || { say "T1 log $T1_LOG does not exist"; STATUS=T1_SILENT; exit 1; }
    until grep -q '^=== time_varying_netns ended' "$T1_LOG"; do
        now=$(date +%s)
        if [ "$now" -ge "$deadline" ]; then
            STATUS=T1_WAIT_TIMEOUT
            say "T1 did not end by $T1_DEADLINE: last line: $(tail -1 "$T1_LOG" | cut -c1-200)"
            exit 1
        fi
        if [ $((now - $(stat -c %Y "$T1_LOG"))) -gt 1200 ]; then
            STATUS=T1_SILENT
            say "T1 log not written for over 20 min and no ended line: last line: $(tail -1 "$T1_LOG" | cut -c1-200)"
            exit 1
        fi
        sleep 30
    done
    say "T1 ended: $(grep '^=== time_varying_netns ended' "$T1_LOG" | tail -1)"
    sleep 30 # its netns teardown and reader
fi
if [ "${STOP_AFTER:-}" = t1 ]; then
    STATUS=STOPPED_AFTER_T1
    exit 0
fi

# 2. unit tests in B (A's the same two as the control, only if B's fail) --------------------------------------------
STATUS=ERROR_TESTS
run_tests() { # run_tests LABEL -> 0 when both build and pass
    local dir="$WTROOT/$1/platform/linux" t rc=0
    say "tests $1: building"
    if ! timeout 900 make -C "$dir" OBJ=obj/ethseed obj/ethseed/tests/test_reliable_pubsub \
        obj/ethseed/tests/test_large_sample >>"$RES/tests.log" 2>&1; then
        say "tests $1: BUILD FAILED"
        return 1
    fi
    for t in test_reliable_pubsub test_large_sample; do
        echo "-- $1 $t --" >>"$RES/tests.log"
        if (cd "$dir" && timeout 900 "./obj/ethseed/tests/$t") >>"$RES/tests.log" 2>&1; then
            say "tests $1: $t PASS"
        else
            say "tests $1: $t FAILED"
            rc=1
        fi
    done
    return $rc
}
if ! run_tests B; then
    if run_tests A; then
        say "A's same tests pass: the failure is B's (the fix or its test edits)"
    else
        say "A's same tests fail too: not the fix alone - read $RES/tests.log"
    fi
    STATUS=TESTS_FAILED
    exit 1
fi

# 3. the A/B, under the box lock taken inside this process -----------------------------------------------------------
STATUS=AB_STEP_FAILED
say "A/B: $REPS reps x A B A2, under RIG_LOCK_SCOPE=box"
# The phase runs A's own copy of this script (the same bytes as the base's, in a worktree nothing else edits), so the
# copy this chain was launched from may be edited or removed meanwhile without touching what runs.
if ! RIG_LOCK_SCOPE=box RIG_LOCK_WAIT=3600 "$WTROOT/A/examples/perf_hil/rig_lock.sh" \
    "$WTROOT/A/examples/perf_hil/experiments/ethernet_seed_ab.sh" __ab >>"$RES/ab_phase.log" 2>&1; then
    say "A/B phase failed: $(tail -2 "$RES/ab_phase.log" | tr '\n' ' ' | cut -c1-300)"
    exit 1
fi
grep -q AB_PHASE_COMPLETE "$RES/chain.log" || { say "A/B phase ended without AB_PHASE_COMPLETE"; exit 1; }

# 4. the reader ------------------------------------------------------------------------------------------------------
STATUS=ERROR_READER
python3 "$WTROOT/A/examples/perf_hil/experiments/ethernet_seed_ab_read.py" "$RES" >"$RES/verdict.txt" 2>&1
VERDICT=$(grep '^ETHSEED_VERDICT ' "$RES/verdict.txt" | tail -1 | cut -d' ' -f2)
[ -n "$VERDICT" ] || { say "the reader printed no verdict: $(tail -3 "$RES/verdict.txt" | tr '\n' ' ')"; exit 1; }
grep -E '^(CHECK|P[0-9]|VOID|SUMMARY|ETHSEED)' "$RES/verdict.txt" | cut -c1-300 | while read -r line; do say "$line"; done
STATUS=VERDICT_$VERDICT
exit 0
