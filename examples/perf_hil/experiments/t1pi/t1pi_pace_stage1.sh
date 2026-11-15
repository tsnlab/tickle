#!/usr/bin/env bash
P=$HOME/tmp_build/pipace/examples/perf_hil/experiments/t1pi
C="tickle tickle-r2 tickle-r4 tickle-r6 tickle-r8 tickle-r12 tickle-r16 tickle-r24"
$P/t1pi_pace.sh stage1_1m 1048576 "$C"
$P/t1pi_pace.sh stage1_128k 131072 "$C"
echo STAGE1_DONE
