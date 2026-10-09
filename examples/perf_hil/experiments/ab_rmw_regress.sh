#!/usr/bin/env bash
# ab_rmw_regress.sh - did rmw_tickle's same-host RTT / CPU (RESULTS R2 / R9) regress between A and B? A rig A/B under
# ONE rig lock, phases A B B A B A A B, each phase rmw_samehost.sh at its commit with arms tickle and cyclonedds (the
# control) on rtt bench block, RELIABLE and BEST_EFFORT (2026-10-09). The reading, written before the lock, is
# ab_rmw_regress.py's docstring (copied to $OUTB.prereg.txt, its sha256 logged under the lock).
#
# Before the lock: rmw_samehost.sh's PC preflight on the two cells and both arms (PREFLIGHT=0 skips), which runs this
# checkout's cell script - the one the rig will run - through the summary.
#
# Usage (detached; ~/rig_results_safe/ab_rmw_regress_<stamp>.*, driver log ends "=== ALL_DONE ... rc=N"):
#   A=<sha> B=<sha> setsid nohup examples/perf_hil/experiments/ab_rmw_regress.sh > <log> 2>&1 < /dev/null &
#   env: PHASES("A B B A B A A B") REPS(3) RTT_DUR(20) PREFLIGHT(1) RIG_LOCK_WAIT(1800)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
A=${A:?A=<sha>}
B=${B:?B=<sha>}
PHASES=${PHASES:-"A B B A B A A B"}
REPS=${REPS:-3}
RTT_DUR=${RTT_DUR:-20}
export RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-1800}
ARMS="tickle cyclonedds"
ROLE=outer
[ "${AB_RMW_REGRESS_INNER:-0}" = 1 ] && ROLE=inner
LOG=/dev/null
on_exit() { # the one EXIT trap
    local rc=$?
    if [ "$ROLE" = outer ]; then
        local v=""
        [ -n "${OUTB:-}" ] && [ -f "$OUTB.verdict.txt" ] && v=" | $(grep -m1 '^VERDICT' "$OUTB.verdict.txt")"
        echo "=== ALL_DONE $(date -Is) rc=$rc${v} ===" | tee -a "$LOG"
    else
        echo "=== ab_rmw_regress under the lock ended $(date -Is) rc=$rc ===" >>"$LOG"
    fi
}
trap on_exit EXIT
trap 'exit 143' TERM INT HUP

if [ "$ROLE" = outer ]; then
    git -C "$REPO" fetch -q origin || echo "warning: git fetch origin failed" >&2
    for v in A B; do
        full=$(git -C "$REPO" rev-parse --verify -q "${!v}^{commit}") || { echo "REFUSED: $v=${!v} does not resolve" >&2; exit 2; }
        [ -n "$(git -C "$REPO" branch -r --contains "$full" 2>/dev/null)" ] || { echo "REFUSED: $v is on no origin branch" >&2; exit 2; }
        printf -v "$v" '%s' "$full"
    done
    OUTB=$HOME/rig_results_safe/ab_rmw_regress_$(date +%Y%m%d-%H%M%S)
    export A B OUTB
    LOG=$OUTB.driver.log
    {
        echo "=== ab_rmw_regress $(date -Is) on $(hostname) ==="
        echo "    A=$A B=$B PHASES='$PHASES' REPS=$REPS RTT_DUR=$RTT_DUR ARMS='$ARMS'"
        echo "    ESTIMATE ~90 min on the rig (8 phases: build 1.5-4 min, dry run ~2 min, $((4 * REPS)) runs of ~28 s)"
    } | tee -a "$LOG"
    python3 "$X/ab_rmw_regress.py" prereg >"$OUTB.prereg.txt" || exit 2
    if [ "${PREFLIGHT:-1}" != 0 ]; then
        echo "--- PC preflight $(date +%T)" | tee -a "$LOG"
        # The PC build is B's (pc_preflight_build.sh at B): the preflight tests the harness, the rig builds each SHA.
        pfb=${PF_BUILD_DIR:-$HOME/pc_preflight_builds/${B:0:12}}
        [ -f "$pfb/BUILD_OK" ] || { echo "REFUSING TO TAKE THE RIG: no PC build at $pfb (PF_BUILD_DIR)" | tee -a "$LOG"; exit 1; }
        PF_TICKLE_INSTALL=$pfb/install PF_PERF_WS=$pfb/perf/install \
            ARMS="$ARMS" PREFLIGHT_ONLY=1 PF_ALLOW_STALE=1 PREFLIGHT_OUT="$OUTB.pf" \
            PF_CELLS="rtt:bench:reliable:block rtt:bench:best_effort:block" "$X/rmw_samehost.sh" >"$OUTB.pf.log" 2>&1 || {
            tail -20 "$OUTB.pf.log"
            echo "REFUSING TO TAKE THE RIG: the PC preflight failed ($OUTB.pf.log)" | tee -a "$LOG"
            exit 1
        }
        grep -q '^F ' "$OUTB.pf.runs"/rtt_bench_reliable_block_tickle_r1/sampler.txt || {
            echo "REFUSING TO TAKE THE RIG: the cell script recorded no fault (F) lines on the PC" | tee -a "$LOG"
            exit 1
        }
    fi
    echo "--- taking the rig lock $(date -Is) (wait up to ${RIG_LOCK_WAIT}s)" | tee -a "$LOG"
    AB_RMW_REGRESS_INNER=1 "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
    exit $?
fi

LOG=$OUTB.driver.log
say() { echo "$*" | tee -a "$LOG"; }
say "=== under the rig lock $(date -Is) ==="
say "    prereg sha256 $(sha256sum "$OUTB.prereg.txt" | cut -c1-64)"
args=()
i=0
for ph in $PHASES; do
    i=$((i + 1))
    sha=$A
    [ "$ph" = B ] && sha=$B
    out="$OUTB.p${i}_$ph"
    say "--- phase $i $ph ${sha:0:8} -> $out ($(date +%T))"
    env SHA="$sha" OUT="$out" ARMS="$ARMS" REPS="$REPS" PREFLIGHT=0 RTT_MSGS=bench TPUT_TOPICS="" \
        QOSES="reliable best_effort" WAITS=block RTT_DUR="$RTT_DUR" "$X/rmw_samehost.sh" >"$out.log" 2>&1
    say "    exit $?; $(grep -c '^  [0-9:]\{8\} ' "$out.txt" 2>/dev/null || echo 0) runs logged"
    args+=("$ph=$out")
done
python3 "$X/ab_rmw_regress.py" read "$A" "$B" "${args[@]}" >"$OUTB.verdict.txt" 2>&1
cat "$OUTB.verdict.txt" >>"$LOG"
say "=== ab_rmw_regress phases done $(date -Is) ==="
