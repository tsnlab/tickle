#!/usr/bin/env bash
# ab_loans.sh - rmw loaned messages (B) against main (A), same-host on one rig Pi, rmw_tickle only, under ONE rig lock.
#
# Why: B (ab/loans) sets can_loan_messages for every type whose wire bytes are its message, which reaches rclcpp's
# default path wherever rclcpp asks the rmw whether it can loan - so it must be shown not to cost that path before it
# lands. What rclcpp actually does with it (jazzy and lyrical read 2026-10-09, confirmed by the PC preflight) is in
# ab_loans.py's header: by default nothing is loaned at all (rcl disables loaned takes unless
# ROS_DISABLE_LOANED_MESSAGES=0, and publish(const T &) never borrows), so the run asks two questions with two rmw arms
# in every phase - D, the shipped defaults (arm tickle), decides the landing; L, loans opted into (arm tickle_loans), is
# reported beside it.
#
# Shape: rmw_samehost.sh once per PHASE (default A B B A B A A B, balanced against a linear and a quadratic drift), each
# phase building its own commit on the Pi, ARMS="tickle tickle_loans", one repetition per cell and arm, cells rtt
# array1k block/poll and tput Array1k / Array4k / RadarDetection (the control: not loanable), both QoS.
#
# HOW TO READ IT - ab_loans.py (its docstring is the pre-registration, written to $OUTB.prereg.txt before the lock and
# its sha256 logged under it): per question, B phases against A phases at df = phases - 2, Bonferroni over the 12
# primaries (rtt p50, delivered/s, cpu us/sample); control 1 (RadarDetection moves either way -> VOID), control 2 (the
# variable under A moves -> VOID), and the treatment counted by rmw_tickle itself (loans taken in every B tickle_loans
# loanable run, none in A or in the control cell, the variable received as set). B lands on D = PASS or IMPROVED.
#
# Before the lock (on this PC; PREFLIGHT=0 skips): rmw_samehost.sh's PC preflight in a private netns, the loans cells,
# both rmw arms, (1) on ~/tickle/install (an A-like build: the treatment must read none) and (2) on a PC build of B
# (PF_B_TICKLE_INSTALL, PF_B_OVERLAYS, PF_B_PERF_TEST - ~/rmw_loans_ab by default, provisioned by its build.sh): the
# treatment must read loans taken in tickle_loans and none elsewhere. Any failure refuses the rig. Under the lock, after
# the first phase of each build the same witness is read on the rig's runs, and a failure stops the job there.
# PC_ONLY=1 stops before the lock (no lock, unpushed commits allowed).
#
# Usage (detached; results in ~/rig_results_safe/ab_loans_<stamp>.*, driver log $OUTB.driver.log, which ends with
# "=== ALL_DONE ... rc=N" however the job ends):
#   A=<sha> B=<sha> setsid nohup examples/perf_hil/experiments/ab_loans.sh > <log> 2>&1 < /dev/null &
#   env: PHASES("A B B A B A A B") RTT_DUR(20) TPUT_DUR(20) PREFLIGHT(1) PC_ONLY(0) RIG_LOCK_WAIT(21600)
#   AB_LOANS_MODE=zcl: both builds lend, B builds loans in the ring slot (ab_loans.py's MODE zcl, its rules).
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
A=${A:?A=<sha> - main}
B=${B:?B=<sha> - the loans head}
PHASES=${PHASES:-"A B B A B A A B"}
RTT_DUR=${RTT_DUR:-20}
TPUT_DUR=${TPUT_DUR:-20}
for ph in $PHASES; do
    case "$ph" in A | B) ;; *) echo "REFUSED: PHASES='$PHASES': only A and B" >&2; exit 2 ;; esac
done
case " $PHASES " in *" A "*" B "* | *" B "*" A "*) ;; *) echo "REFUSED: PHASES='$PHASES' needs A and B" >&2; exit 2 ;; esac
export RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-21600} AB_LOANS_MODE=${AB_LOANS_MODE:-}
RMW_ARMS="tickle tickle_loans"
RTT_MSGS=array1k
TPUT_TOPICS="Array1k Array4k RadarDetection"
PF_CELLS="rtt:array1k:reliable:block rtt:array1k:best_effort:poll tput:Array1k:best_effort tput:Array4k:reliable \
tput:RadarDetection:best_effort"
PF_A_TICKLE_INSTALL=${PF_A_TICKLE_INSTALL:-$HOME/tickle/install}
if [ -z "${PF_A_PERF_WS:-}" ]; then # rmw_samehost.sh's default before it built HEAD (2026-10-09)
    PF_A_PERF_WS=$HOME/rmw_perf_ws/install
    [ -x /tmp/keepall_evict/perf/install/performance_test/lib/performance_test/perf_test ] &&
        PF_A_PERF_WS=/tmp/keepall_evict/perf/install
fi
PF_B_HOME=${PF_B_HOME:-$HOME/rmw_loans_ab}
PF_B_TICKLE_INSTALL=${PF_B_TICKLE_INSTALL:-$PF_B_HOME/rmw/install}
PF_B_OVERLAYS=${PF_B_OVERLAYS:-"$HOME/rmw_loans_ws/rmw/install/local_setup.bash \
$HOME/rmw_loans_ws/ifaces/install/local_setup.bash $PF_B_TICKLE_INSTALL/local_setup.bash \
$PF_B_HOME/perf/install/local_setup.bash"}
PF_B_PERF_TEST=${PF_B_PERF_TEST:-$PF_B_HOME/perf/install/performance_test/lib/performance_test/perf_test}

ROLE=outer
[ "${AB_LOANS_INNER:-0}" = 1 ] && ROLE=inner
LOG=/dev/null
on_exit() { # the one EXIT trap: the outer process always ends the driver log with ALL_DONE, success or not
    local rc=$?
    if [ "$ROLE" = outer ]; then
        local v=""
        [ -n "${OUTB:-}" ] && [ -f "$OUTB.verdicts.txt" ] && v=" | $(tr '\n' ' ' <"$OUTB.verdicts.txt")"
        echo "=== ALL_DONE $(date -Is) rc=$rc${v} ===" | tee -a "$LOG"
    else
        echo "=== ab_loans under the lock ended $(date -Is) rc=$rc ===" >>"$LOG"
    fi
}
trap on_exit EXIT
trap 'exit 143' TERM INT HUP # so a kill still runs the EXIT trap

# ------------------------------------------------------------------------------------------------- outer
if [ "$ROLE" = outer ]; then
    git -C "$REPO" fetch -q origin || echo "warning: git fetch origin failed" >&2
    for v in A B; do
        full=$(git -C "$REPO" rev-parse --verify -q "${!v}^{commit}") || { echo "REFUSED: $v=${!v} does not resolve" >&2; exit 2; }
        if [ "${PC_ONLY:-0}" != 1 ] && [ -z "$(git -C "$REPO" branch -r --contains "$full" 2>/dev/null)" ]; then
            echo "REFUSED: $v=${!v} ($full) is on no origin branch - push it first (PC_ONLY=1 allows it off the rig)" >&2
            exit 2
        fi
        printf -v "$v" '%s' "$full"
    done
    [ "$A" != "$B" ] || { echo "REFUSED: A and B are the same commit" >&2; exit 2; }
    OUTB=$HOME/rig_results_safe/ab_loans_$(date +%Y%m%d-%H%M%S)
    mkdir -p "$(dirname "$OUTB")" || exit 1
    export A B OUTB
    LOG=$OUTB.driver.log
    {
        echo "=== ab_loans $(date -Is) on $(hostname) ==="
        echo "    A=$A"
        echo "    B=$B"
        echo "    PHASES='$PHASES' RMW_ARMS='$RMW_ARMS' RTT_DUR=$RTT_DUR TPUT_DUR=$TPUT_DUR mode=${AB_LOANS_MODE:-loans}"
    } | tee -a "$LOG"
    python3 "$X/ab_loans.py" prereg >"$OUTB.prereg.txt" || exit 2
    # The estimate: a phase is a build on the Pi (a commit switch regenerates the interface packages' TickLE
    # typesupport, ~4 min; the same commit again ~1.5 min), the dry run (~1.5 min) and 20 runs (10 cells x 2 arms) of
    # ~30 s (the run plus the cell's start-up and the fetch; rmw_samehost.sh's own clock, 2026-10-07: ~28 s per run).
    nph=0 sw=0 prev=""
    for ph in $PHASES; do
        nph=$((nph + 1))
        [ "$ph" != "$prev" ] && sw=$((sw + 1))
        prev=$ph
    done
    per_runs=$((20 * (RTT_DUR + TPUT_DUR) / 2 + 20 * 10))
    est=$((sw * 240 + (nph - sw) * 90 + nph * (90 + per_runs)))
    echo "ESTIMATE: ~$((est / 60)) min on the rig ($nph phases, $sw commit switches), plus the lock wait" | tee -a "$LOG"
    if [ "${PREFLIGHT:-1}" != 0 ]; then
        echo "--- PC preflight $(date +%T): harness on ~/tickle/install (A-like: no loans may show)" | tee -a "$LOG"
        # An A-like build is older than this checkout on purpose, so rmw_samehost.sh's stale-build refusal is waived,
        # and it is named here: rmw_samehost.sh's own default is a build of HEAD, which has loans since 7a321d26.
        ARMS="$RMW_ARMS" PREFLIGHT_ONLY=1 PF_ALLOW_STALE=1 PREFLIGHT_OUT="$OUTB.pf_A" PF_CELLS="$PF_CELLS" \
            PF_TICKLE_INSTALL="$PF_A_TICKLE_INSTALL" PF_PERF_WS="$PF_A_PERF_WS" "$X/rmw_samehost.sh" \
            >"$OUTB.pf_A.log" 2>&1 || {
            tail -20 "$OUTB.pf_A.log"
            echo "REFUSING TO TAKE THE RIG: the PC preflight (A-like build) failed ($OUTB.pf_A.log)" | tee -a "$LOG"
            exit 1
        }
        python3 "$X/ab_loans.py" witness "$OUTB.pf_A.runs" A | tee -a "$LOG"
        [ "${PIPESTATUS[0]}" = 0 ] || { echo "REFUSING TO TAKE THE RIG: loans showed on the A-like build" | tee -a "$LOG"; exit 1; }
        if [ ! -f "$PF_B_TICKLE_INSTALL/rmw_tickle/lib/librmw_tickle.so" ] || [ ! -x "$PF_B_PERF_TEST" ]; then
            echo "REFUSING TO TAKE THE RIG: no PC build of B ($PF_B_TICKLE_INSTALL, $PF_B_PERF_TEST) - build one or" \
                "set PF_B_*; PREFLIGHT=0 skips the whole PC preflight" | tee -a "$LOG"
            exit 1
        fi
        echo "--- PC preflight $(date +%T): B's PC build $(md5sum <"$PF_B_TICKLE_INSTALL/rmw_tickle/lib/librmw_tickle.so" | cut -c1-12)" \
            "(loans must show in tickle_loans only)" | tee -a "$LOG"
        ARMS="$RMW_ARMS" PREFLIGHT_ONLY=1 PREFLIGHT_OUT="$OUTB.pf_B" PF_CELLS="$PF_CELLS" \
            PF_TICKLE_INSTALL="$PF_B_TICKLE_INSTALL" PF_OVERLAYS="$PF_B_OVERLAYS" PF_PERF_TEST="$PF_B_PERF_TEST" \
            "$X/rmw_samehost.sh" >"$OUTB.pf_B.log" 2>&1 || {
            tail -20 "$OUTB.pf_B.log"
            echo "REFUSING TO TAKE THE RIG: the PC preflight (B's build) failed ($OUTB.pf_B.log)" | tee -a "$LOG"
            exit 1
        }
        python3 "$X/ab_loans.py" witness "$OUTB.pf_B.runs" B | tee -a "$LOG"
        [ "${PIPESTATUS[0]}" = 0 ] || { echo "REFUSING TO TAKE THE RIG: B's treatment did not show on the PC" | tee -a "$LOG"; exit 1; }
    fi
    if [ "${PC_ONLY:-0}" = 1 ]; then
        echo "PC_ONLY=1: pre-registration written, every PC step passed; no lock taken, nothing run on the rig" | tee -a "$LOG"
        exit 0
    fi
    echo "--- taking the rig lock $(date -Is) (wait up to ${RIG_LOCK_WAIT}s)" | tee -a "$LOG"
    AB_LOANS_INNER=1 "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
    exit $?
fi

# ------------------------------------------------------------------------------------------------- inner, under the lock
LOG=$OUTB.driver.log
say() { echo "$*" | tee -a "$LOG"; }
verdict() { echo "$*" >>"$OUTB.verdicts.txt"; say "$*"; }
: >"$OUTB.verdicts.txt"
say "=== under the rig lock $(date -Is) ==="
say "    prereg sha256 $(sha256sum "$OUTB.prereg.txt" | cut -c1-64) ($OUTB.prereg.txt)"

phase_args=()
witnessed=""
i=0
for ph in $PHASES; do
    i=$((i + 1))
    sha=$A
    [ "$ph" = B ] && sha=$B
    out="$OUTB.p${i}_$ph"
    say "--- phase $i/$(wc -w <<<"$PHASES") $ph ${sha:0:8} -> $out ($(date +%T))"
    env SHA="$sha" OUT="$out" ARMS="$RMW_ARMS" REPS=1 PREFLIGHT=0 RTT_MSGS="$RTT_MSGS" TPUT_TOPICS="$TPUT_TOPICS" \
        RTT_DUR="$RTT_DUR" TPUT_DUR="$TPUT_DUR" "$X/rmw_samehost.sh" >"$out.log" 2>&1
    say "    exit $?; $(grep -c '^  [0-9:]\{8\} ' "$out.txt" 2>/dev/null || echo 0) runs logged"
    phase_args+=("$ph=$out")
    # The first phase of each build: its treatment, read on the rig's own runs, before the rest of the rig time is spent.
    case " $witnessed " in
    *" $ph "*) ;;
    *)
        witnessed="$witnessed $ph"
        python3 "$X/ab_loans.py" witness "$out.runs" "$ph" >>"$LOG" 2>&1 || {
            verdict "VERDICT D: VOID - build $ph's treatment witness failed on the rig in phase $i (see the driver log)"
            exit 1
        }
        ;;
    esac
done
python3 "$X/ab_loans.py" read "${phase_args[@]}" >"$OUTB.verdict.txt" 2>&1
cat "$OUTB.verdict.txt" >>"$LOG"
verdict "$(grep -m1 '^VERDICT D' "$OUTB.verdict.txt" || echo 'VERDICT D: NO VERDICT - the reader printed none')"
verdict "$(grep -m1 '^VERDICT L' "$OUTB.verdict.txt" || echo 'VERDICT L: NO VERDICT - the reader printed none')"
say "=== ab_loans phases done $(date -Is) ==="
