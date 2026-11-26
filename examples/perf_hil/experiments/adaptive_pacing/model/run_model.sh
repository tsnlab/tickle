#!/bin/bash
# Offline model comparison of the candidate designs (DESIGN.md section 26). Run from the repository root.
#   examples/perf_hil/experiments/adaptive_pacing/model/run_model.sh [workdir]
# Builds the scratch harness (the model of tests/test_adaptive_pacing.c with a rate schedule, a repair deferral and a seed,
# plus a copy of pacing_adaptive.h with the candidates behind -DEXP_*), runs 6 seeds of every pattern for each variant in
# the "Pi-like" configuration (round trip samples 1 in 12, reader tick 8 ms, repairs deferred behind the send) and prints
# the means. The library and the tests are not modified.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../../../.." && pwd)
M=$ROOT/examples/perf_hil/experiments/adaptive_pacing/model
W=${1:-$(mktemp -d)}
mkdir -p "$W/inc/tickle"
cp "$ROOT/include/tickle/pacing_adaptive.h" "$W/inc/tickle/pacing_adaptive.h"
(cd "$W/inc" && patch -p1 < "$M/exp_header.patch" > /dev/null) || { echo "exp_header.patch does not apply to this pacing_adaptive.h (it is made against 448b685f)"; exit 1; }
python3 - "$ROOT" "$M" "$W" <<'PY'
import sys,subprocess,os
root,m,w=sys.argv[1:4]
src=open(root+'/tests/test_adaptive_pacing.c').read()
head=src[:src.index('int main(void) {')]
open(w+'/head.c','w').write(head)
subprocess.check_call(['patch','-s','-o',w+'/model_head.c',w+'/head.c',m+'/model_head.patch'])
open(w+'/model_vary.c','w').write(open(w+'/model_head.c').read()+open(m+'/model_main.inc').read())
PY
declare -A V
V[base]=""; V[CC]="-DEXP_CC"; V[ES]="-DEXP_ES"; V[RC25]="-DEXP_RC=25"; V[RC100]="-DEXP_RC=100"; V[RC25CC]="-DEXP_RC=25 -DEXP_CC"
for n in "${!V[@]}"; do
  # shellcheck disable=SC2086 # the variant is a list of -D flags
  gcc -O2 -w ${V[$n]} -DEXP_SPARSE=12 -I"$W/inc" -I"$ROOT/include" -I"$ROOT/tests" "$W/model_vary.c" -o "$W/mx_$n" -lm
done
export DEFER=1
for sd in 1 2 3 4 5 6; do
  for n in "${!V[@]}"; do SEED=$((12345 + sd * 7919)) "$W/mx_$n" all 0 0 8 | grep -v recovery > "$W/os_s8D_${n}_seed$sd.txt"; done
done
cp "$M/aggregate.py" "$W/aggregate.py"
(cd "$W" && python3 aggregate.py s8D base CC ES RC25 RC100 RC25CC)
