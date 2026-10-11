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
cd "$(dirname "${BASH_SOURCE[0]}")/../../.." || exit 1
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
. ./examples/perf_hil/experiments/gate_v2_lib.sh
layout_cflags_default
A=${A:?A=<sha> the parent}; B=${B:?B=<sha> the change}; TAG=${TAG:?TAG=<name>}; REPS=${REPS:-3}
PRIMARY=${PRIMARY:-}
export RIG_LOCK_WAIT=36000
X=./examples/perf_hil/experiments
if [ "${PREFLIGHT:-1}" != 0 ]; then
    mapfile -t pf_specs < <(PREFLIGHT_CELLS_ONLY=1 FWS=tickle $X/campaign_sweep.sh)
    for sha in "$A" "$B"; do
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
for sha in "$A" "$B" "$B" "$A"; do
    n=$((n + 1))
    SHA=$sha FWS=tickle REPS=$REPS OUT=/tmp/${TAG}_b${n}_${sha:0:8}.txt $X/campaign_sweep.sh > "/tmp/${TAG}_b${n}_${sha:0:8}.log" 2>&1
    # The build flags are a treatment: the block's own log must echo the value this chain was given, or the chain stops
    # here rather than read two arms built some other way as the arms it was asked for.
    blk="/tmp/${TAG}_b${n}_${sha:0:8}.log"
    if ! grep -qxF "TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS' (TickLE core and bench; empty: the plain build)" "$blk"; then
        echo "=== chain VOID $(date -Is): block $n's log does not echo TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS' ===" |
            tee "/tmp/${TAG}.done" "/tmp/${TAG}_compare.txt"
        exit 1
    fi
    # One verified line per host and variant: the block's variant list times the two Pis.
    nvar=$(sed -n 's/^--- variants to build: \(.*\) ---$/\1/p' "$blk" | wc -w)
    nver=$(grep -cF "TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS' (" <(grep '^build flags verified: ' "$blk"))
    if [ "$nvar" -eq 0 ] || [ "$nver" -ne $((2 * nvar)) ]; then
        echo "=== chain VOID $(date -Is): block $n has $nver 'build flags verified' lines for $nvar variants on 2 Pis ===" |
            tee "/tmp/${TAG}.done" "/tmp/${TAG}_compare.txt"
        exit 1
    fi
done
cat "/tmp/${TAG}_b1_${A:0:8}.txt" "/tmp/${TAG}_b4_${A:0:8}.txt" > "/tmp/${TAG}_A.txt"
cat "/tmp/${TAG}_b2_${B:0:8}.txt" "/tmp/${TAG}_b3_${B:0:8}.txt" > "/tmp/${TAG}_B.txt"
python3 $X/ab_compare.py campaign "/tmp/${TAG}_A.txt" "/tmp/${TAG}_B.txt" --primary "$PRIMARY" > "/tmp/${TAG}_compare.txt"
echo "=== chain done $(date -Is): $(grep -E '^(VERDICT|NO VERDICT)' "/tmp/${TAG}_compare.txt" | head -1) ===" > "/tmp/${TAG}.done"
