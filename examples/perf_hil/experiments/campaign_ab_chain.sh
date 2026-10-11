#!/usr/bin/env bash
# One TickLE build against another on the rig's native campaign cells, as interleaved A B B A blocks (2026-09-27,
# WIRE_PLAN.md 8.1: the first v10 campaign ran its arms one after the other, so a drift over the session could not be
# told from the change). Each block is campaign_sweep.sh at one SHA, TickLE only, REPS repetitions (default 3, so 6
# per arm), and takes the rig lock itself. Outputs: /tmp/${TAG}_b<n>_<sha8>.txt, then
#   ab_compare.py campaign A.txt B.txt --primary "$PRIMARY"
# reads the arms; /tmp/${TAG}.done is written however the chain ends.
#
# PRIMARY - the pre-registration, and the only thing that decides (the user, 2026-10-06; rules in ab_compare.py's
# header). A chain reads ~40 metrics, and at 2 x SE two or three cross by chance on every run, so the verdict line
# (VERDICT: PASS / WORSE) is taken on the metrics named here alone, each at a Bonferroni threshold for their count:
#   PRIMARY="send_mbps"                         that metric, every role, every cell
#   PRIMARY="c6:server.cpu_s_per_Msample,c10:rtt_avg_ms"   per cell (cN:) and per role (role.) as needed
# Every other metric is printed as secondary and never decides. Without PRIMARY the table is printed and the verdict
# reads "NO VERDICT: no primary metrics pre-registered". Name it before the run, from what the change is meant to
# move - a metric chosen after reading the table is not a primary.
#
# PREFLIGHT (rig_preflight.sh, 2026-10-06): before the first block, BOTH commits are run through the campaign's cell
# shapes on this PC in private network namespaces, and the chain refuses to start if either fails - it never takes the
# rig with a broken harness. The blocks then get PREFLIGHT=0. PREFLIGHT=0 here skips it.
# This script's own checkout, not /home/semih/tickle (2026-10-10): the chain used to cd there, so a queue that checked
# out a harness commit in a runner directory ran the SHARED checkout's campaign_sweep.sh instead. placement_P asked for
# TICKLE_EXTRA_CFLAGS=-falign-functions=64 that way, and the shared sweep did not forward it: both arms built plain, and
# the run read as a placement control that had not been applied. campaign_sweep.sh now says the flags it forwards on
# its own log line (TICKLE_EXTRA_CFLAGS=...), which is the record an arm's treatment is checked against.
#
# GATE V2 A1 (2026-10-11): both arms are built layout-stable - TICKLE_EXTRA_CFLAGS defaults to TICKLE_LAYOUT_CFLAGS
# (gate_v2_lib.sh, -falign-functions=64) when unset, and is exported to every block. A block counts only if its log
# echoes that value AND carries a "build flags verified" line for every TickLE build (campaign_sweep.sh reads each
# build's own traced log with build_flags_check.sh); otherwise the chain stops VOID.
#
# GATE V2 A2/A3 (2026-10-11; the reading is gate_v2.py's header, applied by ab_compare.py campaign --placement):
#   - three arms in mirrored blocks A P B B P A: P (A') is A's commit built with P_EXTRA_CFLAGS (default
#     TICKLE_PLACEMENT_CFLAGS, -falign-functions=128) - every function moved by a multiple of 64 bytes, nothing else -
#     and |P - A| is each metric's placement floor; B is built with B_EXTRA_CFLAGS (default: A's flags), which only the
#     gate's own validation sets (B = A's commit under another alignment: a placement-only "change");
#   - every TickLE process is counted (campaign_sweep.sh INSN_COUNT=1): PRIMARY defaults to executed instructions per
#     sample, user and kernel, every role and cell; WITNESS defaults to cpu_s_per_Msample, send_mbps, recv_mbps,
#     rtt_avg_ms. Both decide; a WORSE must exceed the Bonferroni t threshold AND the placement floor.
#   PRIMARY= / WITNESS= narrow them (same item syntax as above). GATE=v1 runs the old A B B A chain read by the old rule
#   (--primary required), for repeating a chain run before 2026-10-11.
#   Outputs: /tmp/${TAG}_b<n>_<arm>_<sha8>.{txt,log}, /tmp/${TAG}_{A,P,B}.txt, /tmp/${TAG}_compare.txt, /tmp/${TAG}.done.
cd "$(dirname "${BASH_SOURCE[0]}")/../../.." || exit 1
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. ./examples/perf_hil/experiments/gate_v2_lib.sh
layout_cflags_default
A=${A:?A=<sha> the parent}; B=${B:?B=<sha> the change}; TAG=${TAG:?TAG=<name>}; REPS=${REPS:-3}
PRIMARY=${PRIMARY:-}
GATE=${GATE:-v2}
P_EXTRA_CFLAGS=${P_EXTRA_CFLAGS-$TICKLE_PLACEMENT_CFLAGS}
B_EXTRA_CFLAGS=${B_EXTRA_CFLAGS-$TICKLE_EXTRA_CFLAGS}
case "$GATE" in
v2) ARMS="A P B B P A" ;;
v1) ARMS="A B B A" ;;
*) echo "GATE must be v2 or v1" >&2; exit 2 ;;
esac
export RIG_LOCK_WAIT=36000
X=./examples/perf_hil/experiments
flags_of() { case "$1" in A) echo "$TICKLE_EXTRA_CFLAGS" ;; P) echo "$P_EXTRA_CFLAGS" ;; B) echo "$B_EXTRA_CFLAGS" ;; esac; }
sha_of() { case "$1" in A | P) echo "$A" ;; B) echo "$B" ;; esac; }
for arm in A P B; do
    case "$(flags_of $arm)" in *[!A-Za-z0-9=_\ -]*) echo "arm $arm flags may hold only [A-Za-z0-9=_ -]" >&2; exit 2 ;; esac
done
if [ "$GATE" = v2 ] && [ "$P_EXTRA_CFLAGS" = "$TICKLE_EXTRA_CFLAGS" ]; then
    echo "REFUSED: P_EXTRA_CFLAGS equals A's flags ('$TICKLE_EXTRA_CFLAGS'), so P would not perturb placement" >&2
    exit 2
fi
echo "=== campaign_ab_chain $(date -Is) GATE=$GATE TAG=$TAG A=$A B=$B REPS=$REPS blocks: $ARMS" \
    "flags A='$TICKLE_EXTRA_CFLAGS' P='$P_EXTRA_CFLAGS' B='$B_EXTRA_CFLAGS' harness=$(git rev-parse --short HEAD) ==="
if [ "${PREFLIGHT:-1}" != 0 ]; then
    mapfile -t pf_specs < <(PREFLIGHT_CELLS_ONLY=1 FWS=tickle $X/campaign_sweep.sh)
    for sha in $(printf '%s\n' "$A" "$B" | sort -u); do
        if ! SHA=$sha FWS=tickle $X/rig_preflight.sh "${pf_specs[@]}" > "/tmp/${TAG}_preflight_${sha:0:8}.log" 2>&1; then
            tail -25 "/tmp/${TAG}_preflight_${sha:0:8}.log"
            echo "=== chain REFUSED $(date -Is): preflight failed for $sha, see /tmp/${TAG}_preflight_${sha:0:8}.log ===" |
                tee "/tmp/${TAG}.done"
            exit 1
        fi
    done
fi
export PREFLIGHT=0
n=0
for arm in $ARMS; do
    n=$((n + 1))
    sha=$(sha_of "$arm")
    fl=$(flags_of "$arm")
    stem="/tmp/${TAG}_b${n}_${arm}_${sha:0:8}"
    echo "--- block $n arm $arm ${sha:0:8} TICKLE_EXTRA_CFLAGS='$fl' $(date -Is)"
    # Block 1 fails fast (2026-10-11): if its first cell gives no RESULT line, the harness measures nothing on this rig
    # and the chain stops with the reason, instead of running six blocks of "VOID(no RESULT line)" (gate v2's first
    # chains, X1-X3, ~2 h of rig time). Later blocks run every cell: one void there is a row, not a broken harness.
    ff=0
    [ "$n" = 1 ] && ff=1
    TICKLE_EXTRA_CFLAGS=$fl SHA=$sha FWS=tickle REPS=$REPS OUT=$stem.txt FAIL_FAST_FIRST_CELL=$ff \
        $X/campaign_sweep.sh > "$stem.log" 2>&1
    if grep -q '^FAIL FAST: ' "$stem.log"; then
        echo "=== chain VOID $(date -Is): block $n ($arm) stopped at once - $(grep -m1 '^FAIL FAST: ' "$stem.log" | cut -c12-) ===" |
            tee "/tmp/${TAG}.done" "/tmp/${TAG}_compare.txt"
        exit 1
    fi
    # The build flags are a treatment: the block's own log must echo the value this arm was given, or the chain stops
    # here rather than read arms built some other way as the arms it was asked for.
    if ! grep -qxF "TICKLE_EXTRA_CFLAGS='$fl' (TickLE core and bench; empty: the plain build)" "$stem.log"; then
        echo "=== chain VOID $(date -Is): block $n ($arm)'s log does not echo TICKLE_EXTRA_CFLAGS='$fl' ===" |
            tee "/tmp/${TAG}.done" "/tmp/${TAG}_compare.txt"
        exit 1
    fi
    # One verified line per host and variant: the block's variant list times the two Pis.
    nvar=$(sed -n 's/^--- variants to build: \(.*\) ---$/\1/p' "$stem.log" | wc -w)
    nver=$(grep -cF "TICKLE_EXTRA_CFLAGS='$fl' (" <(grep '^build flags verified: ' "$stem.log"))
    if [ "$nvar" -eq 0 ] || [ "$nver" -ne $((2 * nvar)) ]; then
        echo "=== chain VOID $(date -Is): block $n ($arm) has $nver 'build flags verified' lines for $nvar variants on 2 Pis ===" |
            tee "/tmp/${TAG}.done" "/tmp/${TAG}_compare.txt"
        exit 1
    fi
    if [ "$GATE" = v2 ] && ! grep -qx 'INSN_COUNT=1 (perf stat instructions:u/k on every TickLE process; 1 only with FWS=tickle)' \
        "$stem.log"; then
        echo "=== chain VOID $(date -Is): block $n ($arm) did not count instructions (no INSN_COUNT=1 line) ===" |
            tee "/tmp/${TAG}.done" "/tmp/${TAG}_compare.txt"
        exit 1
    fi
    cat "$stem.txt" >>"/tmp/${TAG}_${arm}.new"
done
for arm in A P B; do [ -f "/tmp/${TAG}_${arm}.new" ] && mv "/tmp/${TAG}_${arm}.new" "/tmp/${TAG}_${arm}.txt"; done
if [ "$GATE" = v2 ]; then
    python3 $X/ab_compare.py campaign "/tmp/${TAG}_A.txt" "/tmp/${TAG}_B.txt" --placement "/tmp/${TAG}_P.txt" \
        --primary "$PRIMARY" ${WITNESS+--witness "$WITNESS"} > "/tmp/${TAG}_compare.txt"
else
    python3 $X/ab_compare.py campaign "/tmp/${TAG}_A.txt" "/tmp/${TAG}_B.txt" --primary "$PRIMARY" > "/tmp/${TAG}_compare.txt"
fi
echo "=== chain done $(date -Is): $(grep -E '^(VERDICT|NO VERDICT)' "/tmp/${TAG}_compare.txt" | head -1) ===" > "/tmp/${TAG}.done"
