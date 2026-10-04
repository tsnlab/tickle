#!/usr/bin/env bash
# Runs clock_cost.c on the rig's client Pi, pinned to the bench's client core, 3 reps. Reading rule: the
# clock term per sample is 4 x clock_cost_ns (four reads per publish); it is a material share of the
# publisher's 0.655 us of user time per sample only if that product exceeds 10% (65 ns).
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
K=$HOME/.ssh/tickle_ci_ed25519
HOST=${HOST:-10.1.1.214}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
sh_ "mkdir -p /tmp/clockcost && cat > /tmp/clockcost/clock_cost.c" <"$REPO/examples/perf_hil/experiments/clock_cost.c"
sh_ "cd /tmp/clockcost && gcc -O2 -o clock_cost clock_cost.c && for r in 1 2 3; do taskset -c 2 ./clock_cost; done" </dev/null
