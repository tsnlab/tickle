#!/usr/bin/env bash
# Runs recv_single_cost.c on the rig's server Pi (Cortex-A76), pinned to one core. The x86 numbers in
# that file's header say what one receive call costs there; this asks the same of the rig's CPU, which
# is where c10's +6 us round trip on ef0c7ea0 was measured.
#
# Take the rig lock around it: RIG_LOCK_SCOPE=hil examples/perf_hil/rig_lock.sh <this script>
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
K=$HOME/.ssh/tickle_ci_ed25519; SERVER=10.1.1.213
echo "=== recv_single_cost on $SERVER, $(date -Iseconds), source md5 $(md5sum "$HERE/recv_single_cost.c" | cut -c1-12) ==="
scp -q -i "$K" -o BatchMode=yes "$HERE/recv_single_cost.c" "ci@$SERVER:/tmp/rsc.c"
ssh -i "$K" -o BatchMode=yes "ci@$SERVER" 'uname -m; cc -O2 -o /tmp/rsc /tmp/rsc.c && taskset -c 2 /tmp/rsc'
echo "=== done ==="
