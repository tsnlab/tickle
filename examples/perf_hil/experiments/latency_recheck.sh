#!/usr/bin/env bash
# Re-measure §3's two latency cells after 0f536579 gave the latency servers the +15s safety-cap
# buffer. The comparison_resweep.sh run of 2026-09-24 deployed 659013e9, before that fix, so its
# TickLE latency cells (47/100 answered) measured the harness, not TickLE. The DDS cells from that
# run are unaffected (their servers take no -d) and stay valid.
#
# HOW TO READ IT, written before running: TickLE must now answer ~100/100 at -i 0.1 -d 10. If it
# still loses ~half, the fix did not reach the rpis or the cause is something else - look at the
# server log span again before reading anything else. DDS runs alongside as the control, with the
# same interleaving as the re-sweep.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PH/../.." && pwd)"
if [ "${RIG_LOCK_HELD_HIL:-${RIG_LOCK_HELD:-0}}" != "1" ]; then
    exec "$PH/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi
SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
HOSTS=(10.1.1.214 10.1.1.213)
REPS="${REPS:-3}"
OUT="${OUT:-/tmp/tickle_latency_recheck_$(date +%Y%m%d-%H%M%S).txt}"
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
ssh_h() { local h=$1; shift; ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$h" "$@"; }

git -C "$REPO" fetch -q origin
SHA="$(git -C "$REPO" rev-parse origin/main)"
say "=== latency recheck, $(date -Is), main $SHA, ${REPS} reps ==="
for h in "${HOSTS[@]}"; do
    ssh_h "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx
cd examples/perf_hil && for fw in tickle cyclonedds fastdds; do for s in best_effort_latency reliable_latency; do (cd \$fw && ./build.sh \$s >/tmp/recheck_build.log 2>&1) || { echo BUILD FAILED \$fw \$s; exit 1; }; done; done" &
done
wait
for scen in best_effort_latency reliable_latency; do
    for rep in $(seq 1 "$REPS"); do
        for fw in tickle cyclonedds fastdds; do
            res=$(cd "$PH/$fw" && timeout 120 ./run_scenario.sh "$scen" -i 0.1 -d 10 2>/dev/null | grep '^RESULT:' | grep -v role=server | sed -E 's/RESULT: framework=[a-z]+ scenario=[a-z_]+ //' || true)
            say "$scen rep$rep $fw | ${res:-NO RESULT LINE}"
        done
    done
done
say "=== done $(date -Is) ==="
