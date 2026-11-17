#!/usr/bin/env bash
# shellcheck disable=SC1091,SC2029,SC2016,SC2086,SC2129,SC2012,SC2024,SC2034,SC2209  # experiment driver: ssh-side expansion, unquoted word lists and sudo redirects are deliberate
# Detached chain of the pacing plan after stage 1 (PACE_PLAN.md sections 2-5): diagnosis runs, stage 2 (choices by t1pi_pacread.py --pick),
# final comparison. Resumable: every block skips finished runs.
P=$HOME/tmp_build/pipace/examples/perf_hil/experiments/t1pi
B=$HOME/rig_results_safe/t1pi_pace_20261115
L=$B/chain.log
log() { echo "$(date -Is) $*" >>"$L"; }
until grep -q STAGE1_DONE "$B/stage1.log" 2>/dev/null; do sleep 60; done
log "stage1 done"
eval "$(python3 $P/t1pi_pacread.py --pick 1m=$B/stage1_1m 128k=$B/stage1_128k | sed 's/^/export /')"
log "picked R1=$R1 R2=$R2 FINAL=$FINAL BAND=$BAND"
# diagnosis (unscored): 1 MiB, pcap, unpaced o200 x3, ctl x1, o60 x1, paced FINAL o200 x2
for arm in o200 o60 ctl; do
    n=1; [ $arm = o200 ] && n=3
    OUT=$B/diag CELLS=rel_large ARMS=$arm ROUNDS=$n COPIES=A ROTATE=0 FWS="tickle tickle-r$FINAL" LARGE_SZ=1048576 DUR=60 PCAP=1 $P/t1pi_fw.sh >>"$L" 2>&1
done
log "diag done"
# stage 2: burst variants and fragmentation modes, both sizes
C2="tickle-r$R1-b1538 tickle-r$R1-b24608 tickle-r$R2-b1538 tickle-r$R2-b24608 tickle-roff-fip tickle-roff-fmtu tickle-r$R1-fip tickle-r$R1-fmtu"
$P/t1pi_pace.sh stage2_1m 1048576 "$C2" >>"$L" 2>&1
$P/t1pi_pace.sh stage2_128k 131072 "$C2" >>"$L" 2>&1
log "stage2 done"
# final comparison: paced TickLE (FINAL), unpaced TickLE reference, CycloneDDS, Fast DDS
CF="tickle tickle-r$FINAL cyclonedds fastdds"
$P/t1pi_pace.sh final_1m 1048576 "$CF" >>"$L" 2>&1
$P/t1pi_pace.sh final_128k 131072 "$CF" >>"$L" 2>&1
$P/t1pi_pace.sh final_small 131072 "$CF" rel_small >>"$L" 2>&1
log "CHAIN_DONE"
