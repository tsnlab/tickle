#!/usr/bin/env bash
# Is DATA_FRAG's 8-slot reassembly pool what holds c6's throughput retention at ~70% (target 90.6%)?
# Plan's hypothesis (2026-09-26): an 8-slot pool at c6's rate evicts incomplete reassemblies constantly,
# since new samples' fragments keep arriving while old ones wait for their retransmission. Dosed off the
# rig first, on veth (veth_c6_check.sh), so the rig run can be sized by what this shows.
#
# Arms are p4 reliable_throughput builds differing only in tt_FRAG_REASSEMBLY_SLOTS
# (build.sh TICKLE_FRAG_SLOTS), each copied to its own directory beforehand; every rep runs every arm at
# 0% and 5% loss, interleaved, so drift over the run lands on all arms alike.
#
# HOW TO READ IT, written before running:
#   Every arm must report its own frag_slots= and sample_path=frag. A row that does not is void.
#   CONTROL, 0% loss: frag_abandoned ~0 and the same send_mbps in every arm. If more slots move the
#   lossless rate, the build differs in something besides the pool, and the lossy rows say nothing.
#   At 5%:
#     frag_abandoned falls with slots AND retention (5% send_mbps / 0% send_mbps) rises with it,
#     packets per sample falling below the 8-slot arm's
#       -> the pool is the bottleneck; the smallest N that closes the gap is the one to keep.
#     frag_abandoned falls with slots while retention and packets per sample stay put
#       -> the pool is not the story. Dev's prediction: a sample that lost a fragment is resent whole
#          whatever the pool held, so evicting its partial slot costs nothing, and abandonments are
#          lost-fragment samples being counted, not samples being lost.
#   veth is not the rig: read direction and size, not absolute Mbit/s.
#
# Usage: veth_frag_slots_dose.sh <reps> <arm dir>...   Results: $OUT (default /tmp/veth_frag_slots_dose.txt)
set -u
REPS=${1:?usage: veth_frag_slots_dose.sh <reps> <arm dir>...}
shift
OUT=${OUT:-/tmp/veth_frag_slots_dose.txt}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: >"$OUT"
for rep in $(seq 1 "$REPS"); do
    for arm in "$@"; do
        for loss in 0 5; do
            line=$("$HERE/veth_c6_check.sh" "$arm" "$loss" 2>&1 | tail -2 | tr '\n' ' ')
            fields=$(grep -h "RESULT" /tmp/veth_c6_server.log | tr ' ' '\n' |
                grep -E "^(frag_slots|frag_reassembled|frag_abandoned|frag_dropped|recv|lost)=" | paste -sd' ')
            echo "rep=$rep arm=$(basename "$arm") $(echo "$line" | grep -oE "loss=[0-9]+% samples=[0-9]+ tx_packets_per_sample=[0-9.]+|send_mbps=[0-9.]+|drained=[a-z]+|sample_path=[a-z]+" | sort -u | paste -sd' ') $fields" >>"$OUT"
        done
    done
done
echo "DONE" >>"$OUT"
