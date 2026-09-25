#!/usr/bin/env bash
# §3a's "Paced to ~8 Mbps" table, re-measured at the current main in one session, all three
# frameworks interleaved (the max-rate table was re-measured by comparison_resweep.sh part B).
#
# Pacing: reliable_throughput -i 0.000075 s, which is ~13.3k msg/s of the 76-byte Bench shape, or
# ~8.1 Mbps, the rate the 2026-09-23 table reports for TickLE. The same -i goes to all three
# clients: each sends one sample per interval, then -d 8.
# TickLE runs at its default (KEEP_LAST, depth 64) and the DDS pair at their default
# max_blocking_time, which are the paced table's columns.
#
# HOW TO READ IT, written before running:
#   - A column that does not reach ~8 Mbps at 0% is not "keeping up", and its lossy cells are then
#     about rate, not recovery. That column is read against its own 0% row.
#   - Net loss = lost - write_fail. DDS refusals are counted separately, as in the max-rate table.
#   - A cell that did not end drained=acked is marked, and its Mbps counts acceptance.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PH/../.." && pwd)"
if [ "${RIG_LOCK_HELD_HIL:-${RIG_LOCK_HELD:-0}}" != "1" ]; then
    exec "$PH/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi
SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT=10.1.1.214
HOSTS=(10.1.1.214 10.1.1.213)
REPS="${REPS:-3}"
IV="${IV:-0.000075}"
OUT="${OUT:-/tmp/tickle_paced_resweep_$(date +%Y%m%d-%H%M%S).txt}"
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
ssh_h() { local h=$1; shift; ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$h" "$@"; }
sl() { if [ "$1" = 0 ]; then ssh_h "$RPI_CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true
       else ssh_h "$RPI_CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss $1%"; fi; }
trap 'sl 0' EXIT
git -C "$REPO" fetch -q origin
SHA="$(git -C "$REPO" rev-parse origin/main)"
say "=== paced re-sweep, $(date -Is), main $SHA, -i $IV, ${REPS} reps ==="
for h in "${HOSTS[@]}"; do
    ssh_h "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx
cd examples/perf_hil && for fw in tickle cyclonedds fastdds; do (cd \$fw && ./build.sh reliable_throughput >/tmp/paced_build.log 2>&1) || { echo BUILD FAILED \$fw; exit 1; }; done" &
done
wait
for pct in 0 1 5 20 50; do
    sl "$pct"
    for rep in $(seq 1 "$REPS"); do
        for fw in tickle cyclonedds fastdds; do
            res=$(cd "$PH/$fw" && timeout 180 ./run_scenario.sh reliable_throughput -d 8 -i "$IV" 2>/dev/null | grep '^RESULT:' | tr '\n' ' ' | sed -E 's/RESULT: framework=[a-z]+ scenario=[a-z_]+ //g' || true)
            say "loss$pct rep$rep $fw | ${res:-NO RESULT LINE}"
        done
    done
done
sl 0
say "=== done $(date -Is); tc restored ==="
