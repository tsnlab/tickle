#!/usr/bin/env bash
# holdoff_ab.sh - stage S3 of the reorder hold-off: the screening A/B of every candidate against off and main, between
# private network namespaces on this PC, through time_varying_netns.sh. 2026-11-04. No rig, no default namespace.
# Design: ~/claude_reports/REORDER_HOLDOFF_DESIGN_2026-11-03.md section 4 (4.0 S3 row, 4.1 arms, 4.2 cells, 4.3
# metrics, 4.4 the best rule, 4.5 harness vs link reorder, 4.6 VOID, 4.7 time). holdoff_ab_read.py applies every rule
# below IN CODE; this header is what it was written from, and the reader, not this comment, decides.
#
# TREES. Each is a `git archive` of one commit into OUT/trees/<name> (frozen: an edit in a shared worktree during the
# run cannot reach it), with this checkout's time_varying_netns.sh, time_varying_read.py and wifi_burst_read.py laid
# over it (the harness is not the code under test; the bench nodes and the core stay the tree's own):
#   main = MAIN_REF (default 6e33e811, tr-fix: main ad4f7fe8 + the harness's one-datagram-per-skb default). Its nodes
#          have no hold-off and print no HOLDOFF line.
#   ab   = AB_REF (default bccfdea9, ab/reorder-holdoff): the hold-off behind the A/B seam. Its nodes read
#          TT_HOLDOFF_POLICY and print "HOLDOFF: mode= policy= value_ns= env=" (holdoff_env.h).
#   The harness refuses to start if main has holdoff_env.h or ab has not (the tree would not run what the arm names).
#
# ARMS (user decision (i), 2026-11-04: auto must equal off exactly on a link that never reorders, so the ungated
# candidates A, A' and C are out): main (the main tree; the A/A control of off), off, f250us, f1ms, f4ms, f12ms (fixed
# holds), B, D, E (gated auto candidates). Every arm but main runs the ab tree with NODE_ENV="TT_UDP_OFFLOAD=0
# TT_HOLDOFF_POLICY=<arm>"; main runs with NODE_ENV="TT_UDP_OFFLOAD=0" (netem must drop one datagram per skb).
#
# CELLS (time_varying_netns.sh arms, PROFILE=ethernet, data cell rel_large; DATA_CELLS adds rel_small):
#   pinned, NODE_PIN="2 4 6" (X0a: unpinned veth reorders on this PC): s_l0 (as s_l0_pin, the 4.5 control: the clean
#     1 ms link with the harness reorder pinned away), s_j0 (6 ms, no reorder: "equal to off"), s_j5, J_p2, J_p10
#     (jitter reorder; J's 2 s and 10 s cycles: decay and re-entry), s_lb5, s_lg5 (1 ms loss), s_d20 (long-RTT loss:
#     the gate must not open), s_r1, s_r6 (link reorder, netem "reorder 1%" at 1 ms and 6 ms: the statistical cells).
#   unpinned (NODE_PIN=""): s_l0 alone - the cell that deliberately keeps the harness reorder (1-8 gaps a rep: read
#     0 against non-0 only, never ranked).
#   One time_varying_netns.sh invocation per (arm, rep, pinned|unpinned), REPS=1 each, into
#   OUT/runs/<arm>.r<rep>.<pin|free>/ (a complete time_varying OUT with its own summary and s_l0 control).
#
# ORDER. Within each rep every arm runs once; the arm order is rotated by rep*ceil(arms/REPS) and reversed on even
# reps, so a drift over the night (other sessions, thermal) lands on every arm at a different position, not on the
# last arms alone. order.txt records the order actually run.
#
# PRE-REGISTERED (2026-11-04, before any run; holdoff_ab_read.py's constants enforce each one):
#   Metrics per run (4.3): M1 copies after delivery / delivered unit ((large_duplicate + duplicates + late_below_ack)
#     / (delivered x 723)), the primary. M2 retransmitted / unicast tc drops at the writer (n/a under 10 drops). M3
#     one-way p50/p99 and deadline goodput (200 ms rel_large, 100 ms rel_small). M4 the writer's last PEER_P p
#     against the realized loss (report). M5 nack_spurious, reorder_fill_original, reorder_absorbed, holdoff engaged/
#     released/early, acknack_held, gate_opened. M6 CPU ms/MB both sides, ACKNACK count and bytes (report).
#   Sample counts first: a run delivering or offering under 100 rel_large samples is VOID; an (arm, cell) needs 2
#     usable reps, else every rule reading it is VOID.
#   VOID (outcomes, never configuration; 4.6):
#     V-I  off's nack_spurious differs from its large_duplicate by over 5% on any reorder cell with >= 50 duplicates
#          -> the S0 instrument is wrong and EVERY verdict is VOID; no such cell at all -> also VOID (not checked).
#     V-T  a run whose HOLDOFF lines (ping, echo and idle node, one each) are missing or name another mode/policy/
#          value/env than its arm, a main run that prints one, a non-main run without the reader's holdoff RSTATS,
#          off with holdoff engaged != 0, or a fixed/auto arm with engaged == 0 on a statistical reorder cell -> the
#          run is VOID ("an arm must report its treatment"). off's s_j5 M1 median under 50% -> jitter not applied,
#          s_j5 J_p2 J_p10 VOID. off's s_r1 gaps_opened per rep outside [0.5, 1.5] x offered units x 1% -> s_r1 VOID
#          (amended from the design's reorder_fill_original, before any run: that counts seq_nos, and one netem
#          reorder event fills every datagram it overtook, hundreds at 1 ms; a gap is opened once per event).
#     V-C  main vs off (A/A), per cell and metric, |main - off| / max(off, floor) over 10% -> that metric is VOID in
#          that cell; fewer than 2 usable reps of either -> the cell is VOID. An invocation whose s_l0 control lost
#          anything (time_varying's rule) -> every run of that invocation VOID. The time_varying run VOIDs (RESULT
#          missing, no match, units, short trace, phases) carry over; a loaded PC voids a run's latency only.
#   Tolerance per cell and metric: tol = max(10%, 2 x the A/A difference).
#   Gates (4.4.2; a candidate failing any is DISQUALIFIED; a FAIL seen beats a VOID):
#     G1 loss cells s_lb5 s_lg5 s_d20, every data cell: p99 <= off x (1 + tol) and deadline goodput >= off - max(1
#        point, 2 x the A/A difference) (user decision (ii): on loss cells NO WORSE first). A G1 cell VOID -> the
#        candidate is UNDECIDED.
#     G2 reorder-free cells, MEASURED: of s_l0_pin and s_j0, those where off's reorder_fill_original is 0 in every
#        usable rep. Gated candidates (B D E auto): holdoff engaged == 0 exactly. Every candidate: ACKNACKs sent,
#        retransmitted and p99 within tol of off, both ways. No measured reorder-free cell -> G2 VOID.
#     G3 RELIABLE delivers every offered sample in every rep of every cell (or no less than off where off itself
#        misses); stuck-gap warnings a rep <= off's x 1.1 + 1.
#     G4 jump_abandoned_seq and abandoned (hb_abandoned_seq + jump_abandoned_seq + retry_giveups) a rep <= off's x
#        1.1 + 1. (The +1 for G3/G4: counts of 0 or 1 a rep; "not more than off" read exactly fails the A/A itself.)
#   Ranking (4.4.3): score = mean over the statistical reorder cells (s_r1 s_r6 s_j5 J_p2 J_p10; not VOID, off's M1 >
#     0) of (candidate M1 median / off M1 median); at least 2 such cells or the candidate is unranked. Lowest wins;
#     within 10% of the best is a tie, broken by the mean reorder-cell p99 ratio (insn/sample and state bytes are S4/S5
#     measurements, not this harness's). Only QUALIFIED candidates are ranked; the others are printed with scores.
#   Fixed vs auto (4.4.4): if the best fixed hold per reorder cell differs over 4x across cells and no gated candidate
#     scores below the best single fixed arm, the "auto" premise is not supported: recommend that fixed value, to the
#     user. (4.4.5) No candidate qualified -> nothing lands; the copies-vs-p99 table goes to the user.
#   HYPOTHESIS: "a gated hold-off helps the jitter cells without hurting the loss cells". SUPPORTED when a gated
#     candidate is QUALIFIED (G1, G2, G3, G4 PASS) and its mean M1 ratio over the jitter cells (s_j5 J_p2 J_p10) is
#     <= 0.90. FALSIFIED when every gated candidate is DISQUALIFIED or has a jitter ratio > 0.90 (all evaluated: an
#     UNDECIDED candidate keeps it UNDECIDED). A gated candidate whose G2 engaged count is non-zero breaks decision (i)
#     outright, whatever it does on the jitter cells.
#
# TIME (4.7, recomputed): a rel_large run takes DUR + 10 s (40 s measured at DUR=30, X1 smoke 2026-11-03), a small
# run DUR + 7 s, a build of the two nodes 12 s, and every invocation builds. Default 9 arms x 3 reps x (10 pinned + 1
# unpinned cells) = 297 runs, 54 invocations: 9 x 3 x (11 x 40 + 2 x 12) s = 12528 s = 3.5 h. DRY_RUN=1 prints the
# matrix, the order and this estimate for the settings given and exits.
#
# Usage: [MAIN_REF=sha] [AB_REF=sha] [ARMS="main off f250us f1ms f4ms f12ms B D E"] [REPS=3] [DUR=30]
#        [PIN_CELLS="s_j0 s_j5 J_p2 J_p10 s_lb5 s_lg5 s_d20 s_r1 s_r6"] [FREE_CELLS="s_l0"] [DATA_CELLS=rel_large]
#        [NODE_PIN="2 4 6"] [PROFILE=ethernet] [OUT=dir] [DRY_RUN=1] [RIG_LOCK_WAIT=36000] holdoff_ab.sh
#   Launch detached (setsid nohup ... > log 2>&1 < /dev/null &) and watch for the last line, always printed:
#     HOLDOFF_AB_END rc=<n> out=<dir>
#   It takes the box lock (rig_lock.sh, RIG_LOCK_SCOPE=box: this PC) inside itself, around the runs. OUT defaults to
#   ~/rig_results_safe/holdoff_ab/<stamp>; the reading is OUT/read.txt (holdoff_ab_read.py OUT). Needs `sudo -n ip`.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SELF="$HERE/$(basename "${BASH_SOURCE[0]}")"

MAIN_REF=${MAIN_REF:-6e33e8114bfe31d576dbd8d5e1a800ec2725d72d}
AB_REF=${AB_REF:-bccfdea91374efba344519b91d8a0bcb0f145eed}
ARMS=${ARMS:-"main off f250us f1ms f4ms f12ms B D E"}
REPS=${REPS:-3}
DUR=${DUR:-30}
PIN_CELLS=${PIN_CELLS:-"s_j0 s_j5 J_p2 J_p10 s_lb5 s_lg5 s_d20 s_r1 s_r6"}
FREE_CELLS=${FREE_CELLS-"s_l0"}
DATA_CELLS=${DATA_CELLS:-rel_large}
NODE_PIN=${NODE_PIN-"2 4 6"}
PROFILE=${PROFILE:-ethernet}
DRY_RUN=${DRY_RUN:-0}
OUT=${OUT:-$HOME/rig_results_safe/holdoff_ab/$(date +%Y%m%d-%H%M%S)}
RUN_S_LARGE=$((DUR + 10)) # measured 2026-11-03 (X1 smoke): 40 s at DUR=30
RUN_S_SMALL=$((DUR + 7))  # 37 s at DUR=30
BUILD_S=12
OVERLAY=(time_varying_netns.sh time_varying_read.py wifi_burst_read.py)

# The outer process's one EXIT trap, set before anything can refuse, so the END line is always its last line. (The
# inner process, the same file re-executed under the lock, sets its own one below instead.)
T0=$(date +%s)
RC=0
CHILD_PID=""
# shellcheck disable=SC2329 # invoked by the EXIT trap
on_exit() {
    local rc=$?
    [ "$RC" = 0 ] && RC=$rc
    [ -n "$CHILD_PID" ] && kill "$CHILD_PID" 2>/dev/null && wait "$CHILD_PID" 2>/dev/null
    echo "HOLDOFF_AB_END rc=$RC out=$OUT after $(($(date +%s) - T0)) s"
}
if [ "${HOLDOFF_AB_INNER:-0}" != 1 ]; then
    trap on_exit EXIT
    trap 'RC=143; exit 143' TERM INT HUP
fi

# --- validation, shared by both modes ------------------------------------------------------------------------------
[[ $REPS =~ ^[1-9][0-9]*$ && $DUR =~ ^[1-9][0-9]*$ ]] || { echo "REPS and DUR are positive integers" >&2; exit 2; }
for arm in $ARMS; do
    case "$arm" in
    main | off | B | D | E | auto) ;;
    f[0-9]*us | f[0-9]*ms) [[ $arm =~ ^f[1-9][0-9]*(us|ms)$ ]] || { echo "bad fixed arm $arm" >&2; exit 2; } ;;
    *) echo "unknown arm $arm (main off f<N>us f<N>ms B D E auto)" >&2; exit 2 ;;
    esac
done
for c in $DATA_CELLS; do
    case "$c" in rel_large | rel_small) ;; *) echo "DATA_CELLS: rel_large and/or rel_small" >&2; exit 2 ;; esac
done

# with_s_l0 LIST: time_varying_netns.sh adds the s_l0 control to every invocation; the count must include it.
with_s_l0() { case " $1 " in *" s_l0 "*) echo "$1" ;; *) echo "s_l0 $1" ;; esac; }
# shellcheck disable=SC2086 # the list is words on purpose
count() { set -- $1; echo $#; }
read -r -a ARM_LIST <<<"$ARMS"
N_ARMS=${#ARM_LIST[@]}
N_PIN=$(count "$(with_s_l0 "$PIN_CELLS")")
N_FREE=0
[ -n "$FREE_CELLS" ] && N_FREE=$(count "$(with_s_l0 "$FREE_CELLS")")
N_PARTS=1
[ -n "$FREE_CELLS" ] && N_PARTS=2
SECS_ONE=0 # one (arm, rep): its runs and its builds
for c in $DATA_CELLS; do
    s=$RUN_S_SMALL
    [ "$c" = rel_large ] && s=$RUN_S_LARGE
    SECS_ONE=$((SECS_ONE + (N_PIN + N_FREE) * s))
done
SECS_ONE=$((SECS_ONE + N_PARTS * BUILD_S))
N_RUNS=$((N_ARMS * REPS * (N_PIN + N_FREE) * $(count "$DATA_CELLS")))
EST_S=$((N_ARMS * REPS * SECS_ONE))

# order_for_rep REP: the arms of that rep, rotated by (REP-1) x ceil(arms/REPS), reversed on even reps.
order_for_rep() {
    local rep=$1 stride k out=()
    stride=$(((N_ARMS + REPS - 1) / REPS))
    for ((k = 0; k < N_ARMS; k++)); do out+=("${ARM_LIST[$((((rep - 1) * stride + k) % N_ARMS))]}"); done
    if ((rep % 2 == 0)); then
        local rev=()
        for ((k = N_ARMS - 1; k >= 0; k--)); do rev+=("${out[$k]}"); done
        out=("${rev[@]}")
    fi
    echo "${out[*]}"
}

plan() {
    echo "holdoff_ab plan: arms ($N_ARMS) '$ARMS' x reps $REPS x data cells '$DATA_CELLS'"
    echo "  pinned (NODE_PIN='$NODE_PIN'): $(with_s_l0 "$PIN_CELLS") ($N_PIN; s_l0 read as s_l0_pin)"
    [ -n "$FREE_CELLS" ] && echo "  unpinned: $(with_s_l0 "$FREE_CELLS") ($N_FREE)"
    echo "  main tree $MAIN_REF; ab tree $AB_REF; PROFILE=$PROFILE DUR=$DUR"
    local rep
    for rep in $(seq 1 "$REPS"); do echo "  rep $rep order: $(order_for_rep "$rep")"; done
    echo "  estimate: $N_RUNS runs, $((N_ARMS * REPS * N_PARTS)) invocations; (arm, rep) = $SECS_ONE s" \
        "(runs ${RUN_S_LARGE} s rel_large / ${RUN_S_SMALL} s small, builds ${BUILD_S} s each);" \
        "total $EST_S s = $((EST_S / 3600)) h $((EST_S % 3600 / 60)) min; 1.5x deadline $((EST_S * 3 / 2)) s"
}

# tree_check NAME SHA WANT_HOLDOFF: the commit exists, and has holdoff_env.h exactly when the arm needs it.
tree_check() {
    local name=$1 sha=$2 want=$3 has=0
    git -C "$REPO" cat-file -e "$sha^{commit}" 2>/dev/null || { echo "$name tree $sha: no such commit" >&2; return 1; }
    git -C "$REPO" cat-file -e "$sha:examples/perf_hil/tickle/common/holdoff_env.h" 2>/dev/null && has=1
    [ "$has" = "$want" ] || { echo "$name tree $sha: holdoff_env.h present=$has, expected $want" >&2; return 1; }
}

# ======================================================================================================================
# INNER mode: under the box lock (rig_lock.sh re-execs this script with HOLDOFF_AB_INNER=1); runs every invocation.
# ======================================================================================================================
if [ "${HOLDOFF_AB_INNER:-0}" = 1 ]; then
    TV_PID=""
    # shellcheck disable=SC2329 # invoked by the EXIT trap
    on_inner_exit() {
        local rc=$?
        [ -n "$TV_PID" ] && kill "$TV_PID" 2>/dev/null && wait "$TV_PID" 2>/dev/null
        echo "holdoff_ab inner ended rc=$rc $(date -Is)" | tee -a "$OUT/progress.txt"
    }
    trap on_inner_exit EXIT
    trap 'exit 143' TERM INT HUP
    export GIT_CEILING_DIRECTORIES="$OUT/trees" # time_varying's `git -C <tree>` must not find another repository
    failed=0
    for rep in $(seq 1 "$REPS"); do
        for arm in $(order_for_rep "$rep"); do
            if [ "$arm" = main ]; then
                tree="$OUT/trees/main"
                env_words="TT_UDP_OFFLOAD=0"
            else
                tree="$OUT/trees/ab"
                env_words="TT_UDP_OFFLOAD=0 TT_HOLDOFF_POLICY=$arm"
            fi
            for part in pin free; do
                if [ "$part" = pin ]; then
                    cells=$PIN_CELLS
                    pin=$NODE_PIN
                else
                    [ -z "$FREE_CELLS" ] && continue
                    cells=$FREE_CELLS
                    pin=""
                fi
                dir="$OUT/runs/$arm.r$rep.$part"
                echo "START arm=$arm rep=$rep part=$part tree=$(cat "$tree/.tree_sha") $(date -Is)" |
                    tee -a "$OUT/order.txt"
                OUT="$dir" REPS=1 DUR="$DUR" CELLS="$DATA_CELLS" ARMS="$cells" PROFILE="$PROFILE" NODE_PIN="$pin" \
                    NODE_ENV="$env_words" X1=0 EST_TRACE=0 \
                    "$tree/examples/perf_hil/experiments/time_varying_netns.sh" >"$dir.log" 2>&1 </dev/null &
                TV_PID=$!
                wait "$TV_PID"
                rc=$?
                TV_PID=""
                # The invocation's own last line (its EXIT trap always writes it): the job answering, not its exit code.
                end=$(grep -c '^=== time_varying_netns ended rc=0 ' "$dir/summary.txt" 2>/dev/null)
                if [ "$rc" != 0 ] || [ "${end:-0}" != 1 ]; then failed=$((failed + 1)); fi
                echo "END arm=$arm rep=$rep part=$part rc=$rc tv_end_rc0=${end:-0} $(date -Is)" |
                    tee -a "$OUT/order.txt"
            done
        done
    done
    echo "holdoff_ab inner: $failed invocation(s) without a clean end" | tee -a "$OUT/progress.txt"
    [ "$failed" = 0 ] || exit 1
    exit 0
fi

# ======================================================================================================================
# OUTER mode: plan, trees, the lock, the reading, and the END line.
# ======================================================================================================================
plan
tree_check main "$MAIN_REF" 0 || { RC=2; exit 2; }
tree_check ab "$AB_REF" 1 || { RC=2; exit 2; }
for f in "${OVERLAY[@]}"; do [ -f "$HERE/$f" ] || { echo "missing $HERE/$f" >&2; RC=2; exit 2; }; done
if [ "$DRY_RUN" = 1 ]; then
    echo "DRY_RUN=1: nothing created or run"
    exit 0
fi
[ -e "$OUT" ] && { echo "OUT $OUT exists: refusing to mix two runs" >&2; RC=2; exit 2; }
mkdir -p "$OUT/runs" "$OUT/trees" || { RC=1; exit 1; }
plan >"$OUT/plan.txt"
{
    echo "main_sha=$(git -C "$REPO" rev-parse "$MAIN_REF") ab_sha=$(git -C "$REPO" rev-parse "$AB_REF")" \
        "harness_sha=$(git -C "$REPO" rev-parse HEAD) harness_dirty=$(git -C "$REPO" status --porcelain -- \
            examples/perf_hil/experiments | wc -l)"
    echo "arms='$ARMS' reps=$REPS dur=$DUR profile=$PROFILE data_cells='$DATA_CELLS'"
    echo "pin_cells='$PIN_CELLS' free_cells='$FREE_CELLS' node_pin='$NODE_PIN' nproc=$(nproc) est_s=$EST_S"
} >"$OUT/provenance.txt"
for t in main ab; do
    sha=$MAIN_REF
    [ "$t" = ab ] && sha=$AB_REF
    mkdir -p "$OUT/trees/$t"
    if ! git -C "$REPO" archive --format=tar "$sha" src include examples/perf_hil/experiments \
        examples/perf_hil/tickle/common examples/perf_hil/rig_lock.sh | tar -x -C "$OUT/trees/$t"; then
        echo "git archive of $t ($sha) failed" >&2
        RC=1
        exit 1
    fi
    git -C "$REPO" rev-parse "$sha" >"$OUT/trees/$t/.tree_sha"
    for f in "${OVERLAY[@]}"; do
        cp "$HERE/$f" "$OUT/trees/$t/examples/perf_hil/experiments/$f" || { RC=1; exit 1; }
    done
done
(cd "$HERE" && sha256sum "${OVERLAY[@]}") | sed 's/^/overlay /' >>"$OUT/provenance.txt"
echo "holdoff_ab $(date -Is): OUT=$OUT; waiting for the box lock (RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000} s)" |
    tee -a "$OUT/progress.txt"

export MAIN_REF AB_REF ARMS REPS DUR PIN_CELLS FREE_CELLS DATA_CELLS NODE_PIN PROFILE OUT
HOLDOFF_AB_INNER=1 RIG_LOCK_SCOPE=box RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000} \
    RIG_LOCK_OWNER="holdoff_ab.sh pid=$$ out=$OUT $(date -Is)" \
    "$REPO/examples/perf_hil/rig_lock.sh" "$SELF" </dev/null &
CHILD_PID=$!
wait "$CHILD_PID"
RC=$?
CHILD_PID=""
echo "--- reading (holdoff_ab_read.py)" | tee -a "$OUT/progress.txt"
python3 "$HERE/holdoff_ab_read.py" "$OUT" >"$OUT/read.txt" 2>&1
read_rc=$?
tail -n 40 "$OUT/read.txt"
[ "$RC" = 0 ] && RC=$read_rc
exit "$RC"
