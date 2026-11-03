#!/usr/bin/env bash
# time_varying_netns.sh - release stage 3 (time-varying network -> v2.2), T0 of the stage plan
# (~/claude_reports/TIME_VARYING_STAGE_PLAN_2026-11-02.md): the CURRENT build under a link that changes ONE factor at a
# time, between private network namespaces on this PC. 2026-11-02. No rig, no default namespace: every node runs under
# `sudo -n ip netns exec`, and the one EXIT trap removes every namespace this run made.
#
# WHY. wifi_burst_loss_netns.sh's `tv` arm cycles clean -> ge5b4 -> ge10b8 with 20 ms more delay, changing loss and
# delay together. It failed R7 (copies after delivery 15% on ETHERNET, 3.7% on WIFI) but cannot say whether the delay
# step or the loss step did it. Here every time-varying (tv) arm varies one factor, and every phase condition of a tv
# arm is also run as a static arm, so a failure is attributed: to the change (TV) or to the condition itself (STATIC).
#
# TOPOLOGY, NODES, CELLS: as wifi_burst_loss_netns.sh (read its header): three node namespaces (ping ctx 1, echo ctx 2,
# idle ctx 3) bridged in a hub namespace, all shaping on each node's own egress (prio root, broadcast band 1:1 and
# unicast band 1:2, each a netem), fixed MACs and permanent neighbour entries (no ARP), the shared-memory transport
# compiled out; cells be_small and rel_small (wifi_link_node.c, 100-byte samples at 100 Hz, echoed) and rel_large
# (large_sample_bench.c, 1 MiB at LARGE_HZ, one way). Both bands get the same netem in every arm here.
# Differences from that harness, each for a reason:
#   - No netem `slot` in the base (it is Wi-Fi's aggregation jitter; here jitter is a factor of its own, arm J). The
#     base is "limit 10000" plus the phase's delay and loss; NETEM_EXTRA appends words to every phase (e.g. a slot).
#   - Every node writes a per-sample trace: wifi_link_node -t ("seq send_ns recv_ns", arrival order, first copy) and
#     the large bench built with -DR4_TRACE (a "LAT index= sent_ns= arrived_ns=" line per delivered sample; that macro
#     only adds this print in the bench, and no hook exists under it in src/). The large bench is built with
#     -Dtt_RELIABLE_STATS, so copies after delivery are counted on rel_large too.
#   - Phase changes are stamped with CLOCK_MONOTONIC (a helper compiled here), the clock the nodes stamp samples with,
#     just before and just after the change of all three nodes' qdiscs: phases.txt, one line per phase,
#     "PHASE idx=<k> key=<phase> t_before_ns=<ns> t_after_ns=<ns>", the first written when the topology is up. (The
#     old harness's tv_phases.txt is stamped with `date +%s.%N`, CLOCK_REALTIME, and nothing read it.) A static arm
#     writes the same file with its one phase.
#   - The clean static arm s_l0 is in every run: it is the control (below).
#
# PHASES (netem words after "limit 10000"; GE = Gilbert-Elliott as wifi_burst_loss_netns.sh's ge(): 1-k = 0, 1-h = 100%):
#   d1   delay 1ms   loss random ${D_BG_LOSS}%      d20  delay 20ms  loss random ${D_BG_LOSS}%
#   j0   delay 6ms                                  j5   delay 6ms 5ms (uniform +-5 ms: reorders, as jitter does)
#   l0   delay 1ms                                  lb5  delay 1ms loss random 5%
#   lg5  delay 1ms GE L 5% B 4 (WB's ge5b4)         ld2  delay 21ms GE L 10% B 8 (the old tv's third phase, +20 ms)
#   D carries a constant background loss (D_BG_LOSS, default 2%) in both its phases: with no loss a delay step has
#   nothing to repair, so neither V1 (copies after a repair) nor V2 (repair wait) could be exercised. 2% and not 1%:
#   V2 reads a p99, which at 1% loss sits on the edge between repaired and unrepaired samples. The factor varied is
#   still delay alone. J's 6 ms base keeps the mean delay of j0 and j5 equal (1 ms +-5 would clip at 0 and raise it).
#   A step down in delay re-orders: packets queued under the long delay leave after ones enqueued just after the step.
#
# ARMS. A tv arm is <FAMILY>_p<PHASE_S>; every PHASE_S it changes every node's shaping (`tc qdisc change`) to the next
# phase of its family, cycling. A static arm is s_<phase>. FAMILIES (default "D J LB LG LD") x PHASES_S (default
# "2 10") give the tv arms; each family's phases give its static arms, run once and shared (s_l0 by LB, LG and LD,
# s_lg5 by LG and LD):
#   D  d1 -> d20 -> d1 ...   (V1 on the up step, V2 on the down step)
#   J  j0 -> j5 -> j0 ...    (V4)
#   LB l0 -> lb5 -> l0 ...   (V3)
#   LG l0 -> lg5 -> l0 ...   (V3)
#   LD l0 -> lg5 -> ld2 -> l0 ... (today's tv: V1, V2, V3)
# ARMS overrides the list (s_l0 is added if absent).
#
# PRE-REGISTERED (written 2026-11-02 before the first run; time_varying_read.py implements every rule in code and it -
# not this comment - decides; its constants are a DRAFT pending the user, plan decision 2):
#   Each tv arm is judged as (tv value - worst static phase value). If the worst static phase arm already fails the
#   same number, the result is STATIC, not TV, and is out of this stage.
#   V1 (D, LD up step): copies after delivery (core's late_below_ack + duplicates at the reader, RSTATS; rel_large adds
#      large_duplicate) <= 1% of delivered (R7's limit), tv minus worst static.
#   V2 (D, LD down step): the reader's repair wait (one-way delay minus the destination phase's static p50) p99, over
#      samples sent from 2 s after the step to the next step, <= 1.5 x the static arm's own p99. A 2 s phase leaves no
#      such samples: V2 is read at PHASE_S=10 and VOID at 2 (the first 2 s are printed as the transient). The step is
#      timed from the START of the change (t_before; a change took ~65 ms of tc calls before X0b, printed). LD's down step lands
#      in the lossless l0, where nothing is repaired: V2 is N/A there (amended 2026-11-03 after T1, before any re-run).
#   V3 (LB, LG, LD loss steps): deadline goodput (complete samples delivered within a deadline from their creation /
#      offered; later counts as not delivered) of samples sent in the 2 s after each step >= 90% of the worst static
#      phase's deadline goodput (rel_small 100 ms, rel_large 200 ms, drafts; be_small is printed as the channel).
#      Amended 2026-11-02 by user decision after T1: eventual delivery read 100% for RELIABLE and could not fail. The
#      clean control missing the deadline (> 0.1%), or a limit under one window sample, voids V3. The
#      hybrid's n* is not printed by any node; its only reader-side input, the loss code p the publisher holds, is
#      (large bench PEER_P lines): its convergence to the static arm's p is reported, not judged.
#   V4 (J): copies after delivery <= 1% of delivered, tv minus worst static.
#   A/A (user 2026-11-03, after X0c flipped 4 of ~30 verdicts between the same tree's rep halves): V1, V2 and V4 FAIL
#     only when over the limit AND tv - static exceeds the A/A spread (the same quantity on the odd reps against the
#     even reps); over the limit inside the spread is NOISE (undecided). D and LD need 8 usable reps for V1/V2 (run
#     them with REPS=8), the others 4; fewer is VOID. Copy ratios are also printed per run (median and range).
#   rel_large copies are per unit: the denominator is delivered x 723 units (1 MiB at a 1500-byte MTU), checked per run
#   against the publisher's own first-send unit count; a mismatch voids the run (amended 2026-11-03 after T1).
#   VOID (outcome, never config): a missing RESULT line, a Publisher that never matched, under 60% of the offered
#     samples scored, a missing or short trace, a tv run whose phase log has under 2 changes inside its samples' send
#     interval; the clean control s_l0 losing anything (BE < 99.9%, RELIABLE < 100% delivered, any tc drop): the
#     setup drops and every verdict is VOID; s_l0 rel_large resending anything (the clean path reorders; reported as
#     CONTROL_RESENDS): every rel_large verdict is VOID; a treatment not seen in the outcome (D: d20's one-way p50
#     under 15 ms above d1's, per phase in the tv trace too; J: j5's p90-p10 spread under 4 ms above j0's, and in a
#     rel_large tv trace, whose one line per sample is its last unit's arrival, j5's p50 under 3 ms above j0's; a
#     lossy static arm's realized unicast loss outside [0.5 L, 1.5 L]; a tv loss arm whose be_small phases do not show
#     their loss, or with no be_small, no tc drop at all); a static reference that cannot reproduce itself (its two
#     halves differing by more than the rule's own tolerance); too few samples in a window; a PC 1-minute load above
#     nproc/2 at a run's start voids its latency (V2), not its delivery.
#
# X1, THE STATIC CONDITION MAP (added 2026-11-03; ~/claude_reports/TIME_VARYING_EXPERIMENT_PLAN_2026-11-03.md X1). A
# static arm s_x<D>l<L> is "delay <D>ms" plus, for L > 0, "loss random <L>%" (s_x1l0 is s_l0's netem, so the grid uses
# s_l0 for that point). X1=1 sets ARMS to s_l0 and every other point of X1_DELAYS (default "1 3 5 10 20 50 100") x
# X1_LOSSES (default "0 1 2 5 10"): 35 arms with s_l0, and NODE_PIN to "2 4 6" unless it is set (X0a: unpinned veth
# reorders on this PC; NODE_PIN="" runs unpinned on purpose). One run is one PROFILE: X1 is two OUTs, ETHERNET and
# WIFI, read together by `time_varying_read.py --x1 OUT_ETH OUT_WIFI` (a plain read of an OUT with grid arms prints
# the same section after the tv verdicts). Every existing arm, key and rule above is unchanged.
#   PRE-REGISTERED (2026-11-03, before any X1 run; the reader's X1_* constants enforce each one, per profile x cell x
#   metric, on the median over a point's usable reps, and print the first grid point over the limit - lowest delay,
#   then loss - and, per loss, the delays over it). A static FAIL found here is a bug, fixed separately.
#   COPIES (rel cells): copies after delivery <= 1% of delivered (R7 / V1, rel_large per unit as above).
#   DELIVERY: RELIABLE delivers 100% of offered (the s_l0 control's own rule; the small ping drains 3 s, the large
#     sub runs 8 s past the publisher, so a tail repair has time even at 100 ms); BE >= (1 - L) - 4 binomial sigmas
#     of the offered count - 0.1% (CONTROL_BE_MIN's allowance): over 70 BE points ~0.2% chance of one false FAIL.
#   TIMER_PER_GAP (rel cells): repair requests the retry timer sent (the reader's RSTATS acknack_tx timer) per gap it
#     opened (gaps_opened), <= 1.0. A gap's first request is immediate; the timer re-asks only when that request or
#     its repair is lost, 1/(1-L)^2 - 1 = 0.23 a gap at 10% - and once per gap is plan decision 2's cost unit (at
#     most one spurious copy a gap). X0c (c63d12ad): 0.11..0.21 at s_lb5 (1 ms, 5%), 15 at s_d20 (20 ms, 2%): the
#     repair-request storm this map is to locate. Needs 10 gaps a rep, else n/a.
#   RESEND_PER_MISSING (rel cells): units the data writer retransmitted per unit the reader saw missing
#     (missing_opened), <= 1.5 (plan decision 2's "retransmits <= 1.5 x missing"); an ideal ARQ needs 1/(1-L) = 1.11
#     at 10%. Needs 10 missing a rep. Retransmits with no loss are printed (X1 ZERO_LOSS_RESENDS, report only).
#   RTT_P99 (be_small at every L - a lost round trip is not scored -, rel_small at 0%): round-trip p99 <= s_l0's p99
#     + 2 (D - 1) ms + slack, slack = 1 ms + 1% of 2 (D - 1). s_l0's p99 already holds the 1 ms of each way; netem
#     delay is a constant add, and the clean tail is ~0.1 ms (X0c s_l0 p99 - p50 0.08 ms, s_d20 p90 - p50 0.03 ms),
#     so 1 ms is a scheduling hiccup's room at p99 and the 1% the per-packet timer error netem may add over a long
#     delay (not measured here); a HOL wait or a spurious repair round is >= 2 D and exceeds it.
#   OWD_P99: rel_small under loss: R2 (wifi_burst_loss_netns.sh) for independent loss: one-way p99 <= base_owd + T +
#     K x 1.5 x base_rtt, base_owd / base_rtt = s_l0 rel_small one-way / round-trip p50 + (D - 1) / 2 (D - 1),
#     T = 10 ms, K the least k with L x q^k <= 1%, q = 1 - (1 - L)^2 (K = 1 to 5%, 2 at 10%). rel_large at 0%: one-way
#     p99 <= s_l0 rel_large p99 + (D - 1) + 1 ms + 1% of (D - 1).
#   OWD_P50 (rel_large under loss): R4: one-way p50 <= s_l0 rel_large p50 + (D - 1) + 4 x base_rtt.
#   COPIES_RETURN, TIMER_PER_GAP_RETURN, RESEND_PER_MISSING_RETURN (rel_small): the three limits above on the return
#     stream, echo's RELIABLE pongs to ping (ping is that stream's reader, echo its writer). Added before any X1 run
#     but after the smoke (2026-11-03, 50 ms / 5%: 32% copies after delivery on the pongs, none on the data stream,
#     which V1 reads alone).
#   Report only (no agreed limit): RTT_P50, IMMEDIATE_PER_GAP, ONTIME (V3's deadline from creation), ABANDONED (the
#     reader's hb_abandoned_seq + jump_abandoned_seq + retry_giveups, which explains a RELIABLE DELIVERY miss).
#   VOID: the s_l0 rules above (any loss in the control voids every X1 verdict; its rel_large resends void rel_large's);
#     a rep is dropped from its point (printed) when the run voids as above, its realized unicast loss is outside
#     [0.5 L, 1.5 L], or its one-way p50 is under (D - 1) - 0.5 ms - 5% of (D - 1) above s_l0's (the delay not seen); a
#     loaded PC drops a rep's latency metrics only. A point with no usable rep reads V.
#   TIME (measured on the X1 smoke, 2026-11-03, this PC, DUR=30; a run's length is set by DUR, not by the delay):
#     37 s a small run (50 ms / 5% as 1 ms), 40 s a rel_large run, 12 s the build. 35 arms x (2 x 37 + 40) s = 66 min
#     a rep and profile; both profiles: REPS=2 ~4.4 h, REPS=4 ~8.9 h (sequential: one PC, one pinned CPU set).
#
# Usage: [REPS=1] [DUR=30] [FAMILIES="D J LB LG LD"] [PHASES_S="2 10"] [CELLS="be_small rel_small rel_large"]
#        [ARMS=...] [D_BG_LOSS=2] [PROFILE=ethernet|wifi] [NODE_PIN="cpu0 cpu1 cpu2"] [OUT=dir]
#        [X1=1 [X1_DELAYS="1 3 5 10 20 50 100"] [X1_LOSSES="0 1 2 5 10"]] time_varying_netns.sh
#   NODE_PIN runs node k (ping/pub, echo/sub, idle) under taskset on the k-th CPU listed (X0a, the reorder source);
#   each run's meta.txt records it.
#   EST_TRACE=1 builds both binaries with -Dtt_ESTIMATOR_TRACE (X0d): every node prints, at most every 100 ms, one
#   "ESTTRACE t_ns=... ctx=... obj=..." line per writer proxy, reader entry and service client with the estimators'
#   state (tickle.c's estimator_trace()), to stderr - each node's .err file (echo.err, ping.err; the small cells then
#   keep stderr apart from their .log, as rel_large always does). Off by default, and then nothing here changes;
#   provenance.txt and each meta.txt record it. The trace costs a clock read per poll and a write per 100 ms.
#   OUT defaults to ~/rig_results_safe/time_varying/<stamp>: raw logs per run under OUT/runs/<arm>-<cell>-<rep>/, the
#   summary in OUT/summary.txt, which always ends with "=== time_varying_netns ended". READ_ONLY=1 re-reads an OUT.
#   Needs `sudo -n ip` (tc runs under `ip netns exec`). ~45 s a run: the default set is 10 tv + 8 static arms x 3
#   cells = 54 runs a rep, ~40 min.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
REPS=${REPS:-1}
DUR=${DUR:-30}
FAMILIES=${FAMILIES:-"D J LB LG LD"}
PHASES_S=${PHASES_S:-"2 10"}
CELLS=${CELLS:-"be_small rel_small rel_large"}
PROFILE=${PROFILE:-ethernet}
D_BG_LOSS=${D_BG_LOSS:-2}
SMALL_HZ=100
SMALL_BYTES=100
LARGE_BYTES=1048576
LARGE_HZ=${LARGE_HZ:-5}
EST_TRACE=${EST_TRACE:-0}
case "$EST_TRACE" in 0 | 1) ;; *) echo "EST_TRACE must be 0 or 1" >&2; exit 2 ;; esac
TRACE_DEF=()
[ "$EST_TRACE" = 1 ] && TRACE_DEF=(-Dtt_ESTIMATOR_TRACE)
OUT=${OUT:-$HOME/rig_results_safe/time_varying/$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT/runs" || exit 1
SUM="$OUT/summary.txt"
say() { echo "$*" | tee -a "$SUM"; }

if [ "${READ_ONLY:-0}" = 1 ]; then
    python3 "$HERE/time_varying_read.py" "$OUT"
    exit $?
fi
: >"$SUM"

case "$PROFILE" in ethernet | wifi) ;; *) echo "PROFILE must be ethernet or wifi" >&2; exit 2 ;; esac
LARGE_PROFILE=()
[ "$PROFILE" = wifi ] && LARGE_PROFILE=(-P wifi)

# family_phases FAMILY: its phases in cycle order.
family_phases() {
    case "$1" in
    D) echo "d1 d20" ;;
    J) echo "j0 j5" ;;
    LB) echo "l0 lb5" ;;
    LG) echo "l0 lg5" ;;
    LD) echo "l0 lg5 ld2" ;;
    *) return 1 ;;
    esac
}
X1=${X1:-0}
case "$X1" in 0 | 1) ;; *) echo "X1 must be 0 or 1" >&2; exit 2 ;; esac
X1_DELAYS=${X1_DELAYS:-"1 3 5 10 20 50 100"}
X1_LOSSES=${X1_LOSSES:-"0 1 2 5 10"}
if [ "$X1" = 1 ]; then # the X1 grid; (1 ms, 0%) is the control s_l0 itself
    [ -n "${ARMS:-}" ] && { echo "X1=1 sets ARMS; give X1_DELAYS / X1_LOSSES instead" >&2; exit 2; }
    ARMS="s_l0"
    for dl in $X1_DELAYS; do
        for lp in $X1_LOSSES; do
            [[ $dl =~ ^[0-9]+$ && $lp =~ ^[0-9]+$ ]] || { echo "X1_DELAYS/X1_LOSSES are integers" >&2; exit 2; }
            [ "$dl/$lp" = 1/0 ] || ARMS="$ARMS s_x${dl}l$lp"
        done
    done
    NODE_PIN=${NODE_PIN-"2 4 6"} # X0a: unpinned veth reorders on this PC; NODE_PIN="" runs unpinned on purpose
fi
if [ -z "${ARMS:-}" ]; then
    ARMS=""
    STATICS="s_l0"
    for fam in $FAMILIES; do
        phases=$(family_phases "$fam") || { echo "unknown family $fam" >&2; exit 2; }
        for p in $PHASES_S; do ARMS="$ARMS ${fam}_p$p"; done
        for ph in $phases; do
            case " $STATICS " in *" s_$ph "*) ;; *) STATICS="$STATICS s_$ph" ;; esac
        done
    done
    ARMS="$STATICS$ARMS"
fi
case " $ARMS " in *" s_l0 "*) ;; *) ARMS="s_l0 $ARMS" ;; esac

WORK=$(mktemp -d "$HOME/tmp_build/time_varying.XXXXXX") || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
TV_PID=""
on_exit() {
    local rc=$? ns
    [ -n "$TV_PID" ] && kill "$TV_PID" 2>/dev/null
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== time_varying_netns ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
TREE_SHA=$(git -C "$REPO" rev-parse --short HEAD)
DIRTY=$(git -C "$REPO" status --porcelain -- src include platform | wc -l)
say "=== time_varying_netns $(date -Is): tree=$TREE_SHA dirty_paths=$DIRTY PROFILE=$PROFILE REPS=$REPS DUR=$DUR" \
    "ARMS='$ARMS' CELLS='$CELLS' d_bg_loss=$D_BG_LOSS ==="
{
    echo "tree=$TREE_SHA dirty_paths=$DIRTY profile=$PROFILE reps=$REPS dur=$DUR d_bg_loss=$D_BG_LOSS"
    echo "arms='$ARMS' cells='$CELLS' netem_extra='${NETEM_EXTRA:-}' node_env='${NODE_ENV:-}'"
    echo "small_bytes=$SMALL_BYTES small_hz=$SMALL_HZ large_bytes=$LARGE_BYTES large_hz=$LARGE_HZ nproc=$(nproc)"
    echo "est_trace=$EST_TRACE"
    [ "$X1" = 1 ] && echo "x1=1 x1_delays='$X1_DELAYS' x1_losses='$X1_LOSSES' node_pin='${NODE_PIN:-}'"
    echo "build_small=cc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS${TRACE_DEF:+ ${TRACE_DEF[*]}}"
    echo "build_large=cc -O2 -DNDEBUG -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0" \
        "-Dtt_RELIABLE_STATS -DR4_TRACE${TRACE_DEF:+ ${TRACE_DEF[*]}}"
} >>"$OUT/provenance.txt"

# --- build: both binaries from this tree, and the monotonic stamp ---------------------------------------------------
CORE=("$REPO/src/tickle.c" "$REPO/src/encoding.c" "$REPO/src/log.c" "$REPO/src/hal_linux.c")
export TMPDIR=$HOME/tmp_build
if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS "${TRACE_DEF[@]}" -I"$REPO/include" \
    -I"$REPO/src" \
    -I"$REPO/examples/perf_hil/tickle/common" -o "$WORK/node" "$HERE/wifi_link_node.c" "${CORE[@]}" -lpthread -lm \
    >"$WORK/build.log" 2>&1; then
    say "VOID  wifi_link_node did not build"
    tail -5 "$WORK/build.log"
    exit 1
fi
if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    -Dtt_RELIABLE_STATS -DR4_TRACE "${TRACE_DEF[@]}" -I"$REPO/include" -I"$REPO/src" \
    -I"$REPO/examples/perf_hil/tickle/common" \
    -o "$WORK/large" "$HERE/large_sample_bench.c" "${CORE[@]}" -lpthread -lm >>"$WORK/build.log" 2>&1; then
    say "VOID  large_sample_bench did not build"
    tail -5 "$WORK/build.log"
    exit 1
fi
cat >"$WORK/mono.c" <<'EOF'
#include <stdio.h>
#include <time.h>
int main(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("%llu\n", (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec);
    return 0;
}
EOF
if ! cc -O2 -o "$WORK/mono" "$WORK/mono.c" >>"$WORK/build.log" 2>&1; then
    say "VOID  the monotonic stamp did not build"
    exit 1
fi
MONO="$WORK/mono"

# --- shaping -------------------------------------------------------------------------------------------------------
ge() { awk -v L="$1" -v B="$2" 'BEGIN { r = 100 / B; p = L * r / (100 - L); printf "loss gemodel %.4f%% %.4f%%", p, r }'; }
# spec PHASE: the netem arguments of both bands in that phase.
spec() {
    local words
    case "$1" in
    d1) words="delay 1ms loss random ${D_BG_LOSS}%" ;;
    d20) words="delay 20ms loss random ${D_BG_LOSS}%" ;;
    j0) words="delay 6ms" ;;
    j5) words="delay 6ms 5ms" ;;
    l0) words="delay 1ms" ;;
    lb5) words="delay 1ms loss random 5%" ;;
    lg5) words="delay 1ms $(ge 5 4)" ;;
    ld2) words="delay 21ms $(ge 10 8)" ;;
    x*l*) # X1 grid key x<delay ms>l<random loss %>
        [[ $1 =~ ^x([0-9]+)l([0-9]+)$ ]] || return 1
        words="delay ${BASH_REMATCH[1]}ms"
        [ "${BASH_REMATCH[2]}" != 0 ] && words="$words loss random ${BASH_REMATCH[2]}%"
        ;;
    *) return 1 ;;
    esac
    echo "limit 10000 $words ${NETEM_EXTRA:-}"
}
NODES=(p e i)
IPS=(10.78.0.1 10.78.0.2 10.78.0.3)
MACS=(02:78:00:00:00:01 02:78:00:00:00:02 02:78:00:00:00:03)
BCAST=10.78.0.255

topo() {
    local run=$1 k j ns
    HUB="tv$$-$run-h"
    echo "$HUB" >>"$NS_LIST"
    sudo -n ip netns add "$HUB" || return 1
    sudo -n ip -n "$HUB" link add br0 type bridge || return 1
    sudo -n ip -n "$HUB" link set br0 up
    for k in 0 1 2; do
        ns="tv$$-$run-${NODES[$k]}"
        echo "$ns" >>"$NS_LIST"
        NS[k]=$ns
        sudo -n ip netns add "$ns" || return 1
        sudo -n ip -n "$HUB" link add "hp$k" type veth peer name wl0 address "${MACS[$k]}" netns "$ns" || return 1
        sudo -n ip -n "$HUB" link set "hp$k" master br0 && sudo -n ip -n "$HUB" link set "hp$k" up || return 1
        sudo -n ip -n "$ns" addr add "${IPS[$k]}/24" brd + dev wl0 || return 1
        sudo -n ip -n "$ns" link set lo up && sudo -n ip -n "$ns" link set wl0 up || return 1
        shape "$ns" add "$SHAPE_PHASE" || return 1
    done
    for k in 0 1 2; do
        for j in 0 1 2; do
            [ "$j" = "$k" ] && continue
            sudo -n ip -n "${NS[$k]}" neigh replace "${IPS[$j]}" lladdr "${MACS[$j]}" dev wl0 nud permanent || return 1
        done
    done
}
# shape NS add|change PHASE
shape() {
    local ns=$1 how=$2 phase=$3 words
    local t=(sudo -n ip netns exec "$ns" tc)
    words=$(spec "$phase") || return 1
    if [ "$how" = add ]; then
        "${t[@]}" qdisc add dev wl0 root handle 1: prio bands 2 priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 || return 1
        # shellcheck disable=SC2086 # the netem arguments are words on purpose
        "${t[@]}" qdisc add dev wl0 parent 1:1 handle 10: netem $words || return 1
        # shellcheck disable=SC2086
        "${t[@]}" qdisc add dev wl0 parent 1:2 handle 20: netem $words || return 1
        "${t[@]}" filter add dev wl0 parent 1: protocol ip prio 1 u32 match ip dst "$BCAST/32" flowid 1:1 || return 1
        "${t[@]}" filter add dev wl0 parent 1: protocol ip prio 1 u32 match ip dst 255.255.255.255/32 flowid 1:1 ||
            return 1
    else
        # shellcheck disable=SC2086
        "${t[@]}" qdisc change dev wl0 parent 1:1 handle 10: netem $words || return 1
        # shellcheck disable=SC2086
        "${t[@]}" qdisc change dev wl0 parent 1:2 handle 20: netem $words || return 1
    fi
}
# change_all PHASE: every node's two netem qdiscs to PHASE at once - one `tc -batch` per namespace, the three in
# parallel (X0b, 2026-11-03: the six separate tc calls took 62-72 ms a change, this ~15 ms; `sudo -n` allows only ip,
# so tc runs under `ip netns exec`). Fails if any namespace's batch fails.
change_all() {
    local words k batch="$RUN_DIR/phase_batch.tc" pids=() rc=0
    words=$(spec "$1") || return 1
    printf 'qdisc change dev wl0 parent 1:1 handle 10: netem %s\nqdisc change dev wl0 parent 1:2 handle 20: netem %s\n' \
        "$words" "$words" >"$batch" || return 1
    for k in 0 1 2; do
        sudo -n ip netns exec "${NS[$k]}" tc -batch "$batch" &
        pids+=($!)
    done
    for k in "${pids[@]}"; do wait "$k" || rc=1; done
    return $rc
}
# The tv arm's phase changer, a child of this shell (its PID from $!, killed by the trap if still running). Each change
# is stamped before and after all three nodes' qdiscs changed; a failed change is logged as such (the reader voids it).
tv_cycle() {
    local idx=0 k before after ok n=${#TV_PHASES[@]}
    while :; do
        sleep "$PHASE_S"
        idx=$((idx + 1))
        before=$("$MONO")
        ok=1
        change_all "${TV_PHASES[$((idx % n))]}" || ok=0
        after=$("$MONO")
        echo "PHASE idx=$idx key=${TV_PHASES[$((idx % n))]} t_before_ns=$before t_after_ns=$after ok=$ok" \
            >>"$RUN_DIR/phases.txt"
    done
}
counters() {
    local ns=$1 label=$2
    {
        echo "== $label tx_bytes=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/tx_bytes)" \
            "tx_packets=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/tx_packets)" \
            "tx_dropped=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/tx_dropped)" \
            "rx_dropped=$(sudo -n ip netns exec "$ns" cat /sys/class/net/wl0/statistics/rx_dropped)"
        sudo -n ip netns exec "$ns" tc -s qdisc show dev wl0
        echo "neigh $label: $(sudo -n ip -n "$ns" neigh show dev wl0 | tr '\n' ';')"
    } >>"$RUN_DIR/counters.txt"
}
inns() {
    local k=$1
    shift
    local pin=()
    if [ -n "${NODE_PIN:-}" ]; then # X0a: node k on the k-th CPU of NODE_PIN
        local cpus
        read -r -a cpus <<<"$NODE_PIN"
        pin=(taskset -c "${cpus[$k]}")
    fi
    # shellcheck disable=SC2086 # NODE_ENV is words on purpose
    sudo -n ip netns exec "${NS[$k]}" env ${NODE_ENV:-} bash -c 'ulimit -v 2000000; exec "$@"' inns "${pin[@]}" "$@"
}
# trace_err FILE CMD...: CMD with its stderr in FILE under EST_TRACE=1 - the small cells' ESTTRACE lines apart from
# their .log, where a line written to stderr can land inside a buffered stdout line and cut a RESULT or RSTATS line.
# Without EST_TRACE, CMD as it is, its stderr where the caller put it.
trace_err() {
    local file=$1
    shift
    if [ "$EST_TRACE" = 1 ]; then
        "$@" 2>"$file"
    else
        "$@"
    fi
}

run_one() { # run_one ARM CELL REP
    local arm=$1 cell=$2 rep=$3 run="$1-$2-$3" k
    RUN_DIR="$OUT/runs/$run"
    mkdir -p "$RUN_DIR"
    local tv=0
    case "$arm" in
    s_*)
        SHAPE_PHASE=${arm#s_}
        PHASE_S=0
        TV_PHASES=("$SHAPE_PHASE")
        ;;
    *_p*)
        tv=1
        PHASE_S=${arm##*_p}
        # shellcheck disable=SC2207 # the phases are words
        TV_PHASES=($(family_phases "${arm%_p*}")) || { say "VOID  $run: unknown family"; return 1; }
        SHAPE_PHASE=${TV_PHASES[0]}
        ;;
    *)
        say "VOID  $run: unknown arm"
        return 1
        ;;
    esac
    echo "load=$(cut -d' ' -f1 /proc/loadavg) nproc=$(nproc) start=$(date -Is) tv=$tv phase_s=${PHASE_S:-0}" \
        "phases='${TV_PHASES[*]}' spec0='$(spec "$SHAPE_PHASE")' node_pin='${NODE_PIN:-}' est_trace=$EST_TRACE" \
        >"$RUN_DIR/meta.txt"
    if ! topo "$run" >"$RUN_DIR/topo.log" 2>&1; then
        say "VOID  $run: no topology (see $RUN_DIR/topo.log)"
        return 1
    fi
    local t0
    t0=$("$MONO")
    echo "PHASE idx=0 key=$SHAPE_PHASE t_before_ns=$t0 t_after_ns=$t0 ok=1" >"$RUN_DIR/phases.txt"
    local stop="$WORK/stop-$run" mode=be
    rm -f "$stop"
    [ "$cell" != be_small ] && mode=rel
    local common=(-b "$BCAST" -p "$PROFILE" -q "$stop")
    trace_err "$RUN_DIR/idle.err" inns 2 "$WORK/node" idle -c 3 -d $((DUR + 60)) "${common[@]}" \
        >"$RUN_DIR/idle.log" 2>&1 &
    local idle_pid=$!
    TV_PID=""
    if [ "$cell" = rel_large ]; then
        inns 1 "$WORK/large" sub -s "$LARGE_BYTES" -d $((DUR + 8)) -R -b "$BCAST" "${LARGE_PROFILE[@]}" \
            >"$RUN_DIR/echo.log" 2>"$RUN_DIR/echo.err" &
        local echo_pid=$!
        sleep 1
        [ "$tv" = 1 ] && { tv_cycle & TV_PID=$!; }
        # stderr apart (PEER_P lines and core warnings): with 2>&1 a warning lands inside a buffered LAT line
        inns 0 "$WORK/large" pub -s "$LARGE_BYTES" -r "$LARGE_HZ" -d "$DUR" -R -b "$BCAST" "${LARGE_PROFILE[@]}" \
            >"$RUN_DIR/ping.log" 2>"$RUN_DIR/ping.err"
        wait "$echo_pid"
    else
        trace_err "$RUN_DIR/echo.err" inns 1 "$WORK/node" echo -c 2 -d $((DUR + 60)) -m "$mode" -s "$SMALL_BYTES" \
            -r "$SMALL_HZ" -l "$RUN_DIR/owd_us.txt" -t "$RUN_DIR/trace_echo.txt" "${common[@]}" \
            >"$RUN_DIR/echo.log" 2>&1 &
        local echo_pid=$!
        sleep 1
        [ "$tv" = 1 ] && { tv_cycle & TV_PID=$!; }
        trace_err "$RUN_DIR/ping.err" inns 0 "$WORK/node" ping -c 1 -d "$DUR" -m "$mode" -s "$SMALL_BYTES" \
            -r "$SMALL_HZ" -l "$RUN_DIR/rtt_us.txt" -t "$RUN_DIR/trace_ping.txt" "${common[@]}" \
            >"$RUN_DIR/ping.log" 2>&1
        sleep 1
        touch "$stop"
        wait "$echo_pid"
    fi
    if [ -n "$TV_PID" ]; then
        kill "$TV_PID" 2>/dev/null
        wait "$TV_PID" 2>/dev/null
        TV_PID=""
    fi
    sleep 1
    touch "$stop"
    wait "$idle_pid"
    for k in 0 1 2; do counters "${NS[$k]}" "${NODES[$k]}"; done
    for k in 0 1 2; do sudo -n ip netns del "${NS[$k]}"; done
    sudo -n ip netns del "$HUB"
    local line
    line=$(grep -h '^RESULT' "$RUN_DIR/ping.log" "$RUN_DIR/echo.log" | tr '\n' ' ' | cut -c1-300)
    say "$run: phases=$(grep -c '^PHASE' "$RUN_DIR/phases.txt") ${line:-<no RESULT line>}"
}

declare -a NS
declare -a TV_PHASES
for rep in $(seq 1 "$REPS"); do
    for cell in $CELLS; do
        for arm in $ARMS; do
            run_one "$arm" "$cell" "$rep"
        done
    done
done
say "--- reading (time_varying_read.py)"
python3 "$HERE/time_varying_read.py" "$OUT" | tee -a "$SUM"
