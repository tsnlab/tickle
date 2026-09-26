#!/usr/bin/env bash
# Runs wake_granularity.c on both rig Pis to measure G (see the .c file's pre-registration).
# Takes the hil lock: it runs on the rpis, and a concurrent sweep would add exactly the preemption
# this measurement would then mistake for timer granularity.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    exec "$HERE/../rig_lock.sh" "$0" "$@"
fi

OUT="${OUT:-/tmp/wake_granularity_$(date +%Y-%m-%d).txt}"
SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
N="${N:-20000}"

{
    echo "=== wake granularity (G for RFC 6298), $(date -Is) ==="
    echo "source: examples/perf_hil/experiments/wake_granularity.c at $(git -C "$HERE" rev-parse --short HEAD)"
    echo "N=$N per arm, ppoll with no fds, taskset -c 1-3 (same pinning as every harness)"
    echo
} >"$OUT"

for host in 10.1.1.214 10.1.1.213; do
    echo "--- rpi $host ---" >>"$OUT"
    scp -q -i "$SSH_KEY" -o BatchMode=yes "$HERE/wake_granularity.c" "ci@$host:/tmp/wake_granularity.c"
    ssh -i "$SSH_KEY" -o BatchMode=yes "ci@$host" \
        "gcc -O2 -Wall -Wextra -o /tmp/wake_gran /tmp/wake_granularity.c && \
         echo \"  governor=\$(cat /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor 2>/dev/null || echo n/a)\" && \
         taskset -c 1-3 /tmp/wake_gran $N" >>"$OUT" 2>&1
    echo >>"$OUT"
done

echo "=== done ===" >>"$OUT"
