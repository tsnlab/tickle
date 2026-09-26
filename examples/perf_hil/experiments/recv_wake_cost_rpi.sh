#!/usr/bin/env bash
# Runs Dev's recv_wake_cost.c on the rig's server Pi (2026-09-26, RMW_PERF_PLAN.md section 8.1). It measures
# what the receive wait style costs one wake-up - ppoll over three sockets then recvfrom (rmw_tickle's poll
# thread), a blocking recvfrom, and a blocking recvmsg (CycloneDDS's recvUC) - on the Cortex-A76 whose pong
# showed tap -> recv return 5.6 us behind CycloneDDS. On x86 ppoll3 cost +0.8 us. The file's header holds
# the pre-registration. Two passes, receiver and sender on idle cores 2 and 3 (the NIC IRQ is on core 0).
# Take the rig lock: RIG_LOCK_SCOPE=hil examples/perf_hil/rig_lock.sh <this script>
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
K=$HOME/.ssh/tickle_ci_ed25519; SERVER=10.1.1.213
echo "=== recv_wake_cost on $SERVER, $(date -Iseconds), source md5 $(md5sum "$HERE/recv_wake_cost.c" | cut -c1-12) ==="
scp -q -i "$K" -o BatchMode=yes "$HERE/recv_wake_cost.c" "ci@$SERVER:/tmp/rwc.c"
ssh -i "$K" -o BatchMode=yes "ci@$SERVER" 'uname -m; cc -O2 -pthread -o /tmp/rwc /tmp/rwc.c && for pass in 1 2; do echo "-- pass $pass"; /tmp/rwc 2 3; done'
echo "=== done ==="
