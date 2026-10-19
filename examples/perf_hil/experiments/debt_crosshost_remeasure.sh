#!/usr/bin/env bash
# The cross-host measurement debt of ROADMAP "Other open items", re-measured at one pushed commit, unpinned, all three
# frameworks in rotated order (written 2026-10-19, before any of its runs).
#
# WHAT THE DEBT IS (traced 2026-10-19; the ROADMAP line is a one-line summary of older text):
#   "c6 with the `retransmitted` figure" - campaign_sweep.sh cell c6, RELIABLE KEEP_ALL P4 2800 B at 5% loss (RESULTS
#     rows 14, 17, 27, 34, 40, campaign T). COMPARISON 2.8 (2026-09-25, at 6234e915^): "When it is [re-run],
#     tt_Publisher.retransmitted and tt_Subscriber.gap_abandoned/gap_evicted give a free cross-check ... the
#     prediction, written before the run, is retransmitted at about 46x sent and gap_abandoned = 0." c6 has been
#     re-run since (3ae721aa T6, 09a0437d A) and its rows publish, but that reading was never made.
#   "11 ◊ cells in RESULTS (re-measure unpinned)" - the item is COMPARISON to-do 14 (2026-09-23): eth0's IRQ is served
#     on CPU 0 of both Pis, so an unpinned sender that lands there reads ~15% low (1 run in 4), and the
#     reliable_throughput Mbps cells were marked ◊ "until re-measured with enough reps to report both modes". The
#     re-consolidated ROADMAP (2d1c22d3, 2026-10-18) counted "11 marks" by the glyph, but the 11 ◊ in RESULTS at that
#     commit are the DELIVERY FAILED marks of the same-host CycloneDDS cells (the legend: "DELIVERY FAILED ◊"); no
#     RESULTS cell has carried ◊ in the pinned sense since the 2026-10-05 consolidation. The cells the debt is about
#     are the cross-host reliable_throughput cells, c1-c7 and c9 (rows 7-10, 12-17, 18-23, 26-30, 32-40). They were
#     re-measured unpinned in A/T (09a0437d, 3 reps), which cannot report both modes: in that A file TickLE's c1 had
#     one rep on CPU 0 (93.6 Mbps) and two elsewhere (113.8), and most cells saw one mode only.
#
# WHAT IT RUNS. campaign_sweep.sh, unchanged, twice, with the profiles RESULTS takes each row from:
#   T  FASTDDS_PROFILE=fastdds_eth0_only_mms1472.xml, CELLS="3 4 6"   (rows 9, 10, 14, 17, 22, 23, 27, 30, 33, 34,
#                                                                       37, 38, 40)  - first, as c6 is the named debt
#   A  FASTDDS_PROFILE=fastdds_eth0_only.xml,         CELLS="1 2 5 7 9" (rows 7, 8, 12, 13, 15, 16, 18-21, 26, 28, 29,
#                                                                       32, 35, 36, 39, 41)
# REPS (default 12) each, PIN unset (nothing pinned), FWS all three; campaign_sweep.sh rotates the framework order by
# one every cell and every rep. (3/4)^12 = 3%: the chance that a given framework never lands on CPU 0 in a cell.
#
# HOW TO READ IT, written before running and implemented in the readers, not here:
#   - every verdict is campaign_summary.py's on each file (instrument=ok, boundary gate, VOID, delivery, goodput G1-G6,
#     WIN only outside the reps' spread), and fair_crosshost_rows.py maps it to the RESULTS rows (rows of cells not
#     run here are dropped, not shown as MISSING);
#   - debt_crosshost_read.py prints, per cell, the sample counts (N), the realized framework order (O), the placement
#     reading (P1-P3: both modes seen, COIN FLIP SEEN / REVERSED / NOT SEEN / NOT READ, never a re-score), and for c6
#     the retransmitted reading (R1 the 46x prediction, R2 gap_abandoned = gap_evicted = 0, R3 against 3ae721aa).
# Nothing is written into docs/RESULTS.md by this script.
#
# REFUSALS (each prints why, then the DONE line):
#   - SHA unset, not a full 40-hex id, not on origin/main after a fetch, or a fetch that fails ("could not look" is
#     not "yes");
#   - this script's own checkout not at SHA (the harness and the readers must be the measured commit's);
#   - the PC preflight (rig_preflight.sh, every cell, every framework built on this PC, before the lock) failing;
#   - under the lock: a qdisc other than the default (netem or tbf) on either Pi's eth0 - a shaped link would make
#     N0 cells not "no shaping", and with the lock held nobody else may have left it there;
#   - under the lock: not enough time left before NOT_AFTER (default 07:30) for 1.5x the estimate.
#
# THE RIG. The lock (hil) is taken ONCE, inside this detached process, for both campaigns (RIG_LOCK_WAIT default 3600
# s). This script applies nothing to the Pis; campaign_sweep.sh applies and removes netem in its own EXIT trap and
# refuses to start on a link it finds shaped. The PC preflight builds for x86 with TICKLE_EXTRA_CFLAGS and
# GATE_BUILD_ARCH removed from its environment, so the rig's aarch64 layout flags never reach a PC build.
#
# ESTIMATE. PC preflight ~1 min (2026-10-19 at 34db713f: 16 jobs, built in 16 s, PASS in 35 s; FastDDS is not on this
# PC and SKIPs).
# Rig: 8 cells x 3 frameworks x REPS 12 = 288 runs; a throughput run is ~18 s at 0% loss and ~22 s under loss or
# reorder (T 2026-10-08: 18 runs in 6 min 16 s with its build; T6 2026-10-06: 9 c6 runs in 2 min 32 s) -> 5 x 36 x
# 18 s + 3 x 36 x 22 s = 94 min, plus two deploys and builds on both Pis ~4 min each = ~102 min. In all ~110 min;
# 1.5x = 165 min.
#
# Usage (from a detached worktree at SHA, which must be on origin/main; results in ~/rig_results_safe):
#   git -C /home/semih/tickle fetch origin && SHA=$(git -C /home/semih/tickle rev-parse origin/main)
#   git -C /home/semih/tickle worktree add --detach /home/semih/tickle_runner_debt "$SHA"
#   cd /home/semih/tickle_runner_debt && SHA=$SHA setsid nohup examples/perf_hil/experiments/debt_crosshost_remeasure.sh \
#     > ~/rig_results_safe/debt_crosshost_launch.log 2>&1 < /dev/null &
# The last line is always "=== DEBT_CROSSHOST DONE rc=<n> ... ===". DRY_RUN=1 makes the refusals and prints the plan,
# without the preflight, the lock or the rig.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SAFE=$HOME/rig_results_safe
REPS=${REPS:-12}
T_CELLS="3 4 6"
A_CELLS="1 2 5 7 9"
EST_MIN=${EST_MIN:-110}
NOT_AFTER=${NOT_AFTER:-07:30}
DRY_RUN=${DRY_RUN:-0}
SHA=${SHA:-}
say() { echo "$(date -Is) $*"; }

if [ "${DEBT_LOCKED:-0}" != 1 ]; then
    # The one EXIT trap of this (outer) instance: the DONE line, whatever happened. The instance under the lock is a
    # child, not an exec, so a lock that never frees still ends here with a DONE line.
    trap 'echo "=== DEBT_CROSSHOST DONE rc=$? sha ${SHA:-unset} $(date -Is) ==="' EXIT
    case "$SHA" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;;
    *) say "REFUSED: set SHA to the full id of a commit on origin/main (got '${SHA}')"; exit 2 ;;
    esac
    [ ${#SHA} = 40 ] || { say "REFUSED: SHA must be the full 40-hex id, not '${SHA}'"; exit 2; }
    git -C "$REPO" fetch -q origin main || { say "REFUSED: git fetch origin main failed - could not check SHA"; exit 1; }
    git -C "$REPO" merge-base --is-ancestor "$SHA" origin/main 2>/dev/null ||
        { say "REFUSED: $SHA is not on origin/main (the Pis fetch it)"; exit 1; }
    head=$(git -C "$REPO" rev-parse HEAD)
    [ "$head" = "$SHA" ] ||
        { say "REFUSED: this checkout ($REPO) is at $head, not $SHA - run from a detached worktree at SHA"; exit 1; }
    if ! git -C "$REPO" diff --quiet HEAD -- examples/perf_hil; then
        say "REFUSED: $REPO has local changes under examples/perf_hil - the harness would not be $SHA's"
        exit 1
    fi
    python3 "$HERE/debt_crosshost_read.py" --selftest | tail -1 | grep -q '^SELFTEST OK' ||
        { say "REFUSED: debt_crosshost_read.py's selftest does not pass"; exit 1; }
    python3 "$HERE/campaign_summary.py" --selftest | tail -1 | grep -q '^SELFTEST OK' ||
        { say "REFUSED: campaign_summary.py's selftest does not pass"; exit 1; }
    case "$REPS" in '' | *[!0-9]*) say "REFUSED: REPS='$REPS'"; exit 2 ;; esac
    say "=== cross-host debt re-measure: sha ${SHA:0:8}, T cells '$T_CELLS', A cells '$A_CELLS', $REPS reps, unpinned ==="
    say "estimate ~${EST_MIN} min (1.5x $((EST_MIN * 3 / 2)) min); must finish before $NOT_AFTER"
    say "uptime: $(uptime)"
    if [ "$DRY_RUN" = 1 ]; then
        DRY_RUN=1 CELLS="$T_CELLS" REPS=$REPS FASTDDS_PROFILE=fastdds_eth0_only_mms1472.xml SHA=$SHA \
            "$HERE/campaign_sweep.sh" | sed 's/^/  T: /'
        DRY_RUN=1 CELLS="$A_CELLS" REPS=$REPS FASTDDS_PROFILE=fastdds_eth0_only.xml SHA=$SHA \
            "$HERE/campaign_sweep.sh" | sed 's/^/  A: /'
        say "DRY_RUN=1: no preflight, no lock, nothing run"
        exit 0
    fi
    if [ "${PREFLIGHT:-1}" != 0 ]; then
        mkdir -p "$SAFE/preflight"
        mapfile -t specs < <(CELLS="$T_CELLS $A_CELLS" PREFLIGHT_CELLS_ONLY=1 "$HERE/campaign_sweep.sh")
        [ "${#specs[@]}" = 8 ] || { say "REFUSED: expected 8 preflight cell specs, got ${#specs[@]}: ${specs[*]}"; exit 1; }
        pfd=$SAFE/preflight/debt_crosshost_${SHA:0:8}_$(date +%Y%m%d-%H%M%S)
        say "--- PC preflight, ${#specs[@]} cells, every framework this PC has, at ${SHA:0:8} -> $pfd ---"
        printf '      %s\n' "${specs[@]}"
        if ! env -u TICKLE_EXTRA_CFLAGS -u GATE_BUILD_ARCH TMPDIR=/home/semih/tmp_build PREFLIGHT_OUT="$pfd" SHA="$SHA" \
            nice -n 10 "$HERE/rig_preflight.sh" "${specs[@]}" >"$pfd.log" 2>&1; then
            say "REFUSED: PC preflight failed - not taking the rig ($pfd.log):"
            tail -15 "$pfd.log" | sed 's/^/      /'
            exit 1
        fi
        say "--- PC preflight PASS: $(tail -1 "$pfd.log") ---"
    fi
    say "--- taking the rig lock (hil), once for both campaigns (wait at most ${RIG_LOCK_WAIT:-3600} s) ---"
    DEBT_LOCKED=1 SHA=$SHA REPS=$REPS RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-3600} \
        "$REPO/examples/perf_hil/rig_lock.sh" "$HERE/debt_crosshost_remeasure.sh" "$@"
    exit $?
fi

# ---- under the lock (a child of the outer instance; no trap of its own: it applies nothing to the Pis) -----------
say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null | tr '\n' ' ') ---"
K=$HOME/.ssh/tickle_ci_ed25519
for h in 10.1.1.214 10.1.1.213; do
    if ! q=$(ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "tc qdisc show dev eth0" </dev/null 2>&1); then
        say "REFUSED: could not read the qdisc on $h (ssh failed): $q"
        exit 1
    fi
    case "$q" in *netem* | *tbf*)
        say "REFUSED: $h eth0 is shaped with the lock held (a previous run died shaped?): $(tr '\n' ' ' <<<"$q")"
        exit 1 ;;
    esac
    say "link $h eth0 unshaped: $(tr '\n' ' ' <<<"$q" | cut -c1-120)"
done
now=$(date +%s)
deadline=$(date -d "$NOT_AFTER" +%s) || { say "REFUSED: NOT_AFTER='$NOT_AFTER' is not a time"; exit 2; }
[ "$deadline" -gt "$now" ] || deadline=$((deadline + 86400))
if [ $((now + EST_MIN * 90)) -gt "$deadline" ]; then
    say "REFUSED: 1.5x the estimate ($((EST_MIN * 3 / 2)) min) from now passes $NOT_AFTER"
    exit 1
fi
TS=$(date +%Y%m%d-%H%M%S)
OUTB=$SAFE/debt_crosshost_${SHA:0:8}_$TS
run() { # <name> <profile> <cells>
    say "--- $1 (FASTDDS_PROFILE=$2, CELLS='$3', REPS=$REPS, PIN unset) -> $OUTB.$1.txt ---"
    env -u PIN FASTDDS_PROFILE="$2" CELLS="$3" REPS="$REPS" SHA="$SHA" PREFLIGHT=0 OUT="$OUTB.$1.txt" \
        "$HERE/campaign_sweep.sh" >"$OUTB.$1.log" 2>&1
    local rc=$?
    say "    $1 rc=$rc, rows ok $(grep -c ' | ok | ' "$OUTB.$1.txt" 2>/dev/null) of $(grep -cE '^c[0-9]+ .* rep[0-9]+ \| ' "$OUTB.$1.txt" 2>/dev/null)," \
        "last line: $(tail -1 "$OUTB.$1.txt" 2>/dev/null | cut -c1-120)"
    return $rc
}
run T fastdds_eth0_only_mms1472.xml "$T_CELLS"; rc_t=$?
run A fastdds_eth0_only.xml "$A_CELLS"; rc_a=$?

say "--- reading -> $OUTB.reading.txt ---"
{
    echo "=== debt_crosshost_read.py (N counts, O order, P placement, R c6 retransmitted) ==="
    python3 "$HERE/debt_crosshost_read.py" "$OUTB.A.txt" "$OUTB.T.txt"
    echo
    echo "=== RESULTS rows (fair_crosshost_rows.py on A and T; rows of cells not run here dropped) ==="
    python3 "$HERE/fair_crosshost_rows.py" "$OUTB.A.txt" "$OUTB.T.txt" | grep -v ' MISSING$'
    for c in T A; do
        echo
        echo "=== campaign_summary.py $c ==="
        python3 "$HERE/campaign_summary.py" "$OUTB.$c.txt"
    done
} >"$OUTB.reading.txt" 2>&1
grep -E '^   (R[123]|P  .*(COIN FLIP SEEN|REVERSED))|^== totals|^ +[0-9]+[a-c]? [AT] ' "$OUTB.reading.txt" | sed 's/^/  /'
say "campaigns rc: T $rc_t, A $rc_a"
[ "$rc_t" = 0 ] && [ "$rc_a" = 0 ]
