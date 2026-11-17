#!/usr/bin/env bash
# shellcheck disable=SC1091,SC2029,SC2016,SC2086,SC2129,SC2012,SC2024,SC2034,SC2209  # experiment driver: ssh-side expansion, unquoted word lists and sudo redirects are deliberate
P=$HOME/tmp_build/pipace/examples/perf_hil/experiments/t1pi
C="tickle tickle-r2 tickle-r4 tickle-r6 tickle-r8 tickle-r12 tickle-r16 tickle-r24"
$P/t1pi_pace.sh stage1_1m 1048576 "$C"
$P/t1pi_pace.sh stage1_128k 131072 "$C"
echo STAGE1_DONE
