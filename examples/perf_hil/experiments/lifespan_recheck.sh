#!/usr/bin/env bash
# Re-measure §3's lifespan row after a5f41b46 (TickLE Dev). That commit makes the lifespan_expiry
# call sites pass PRE_CLIENT_SLEEP=0, because the server sleeps its -p pause BEFORE creating its
# Subscriber, and a 3 s pre-client sleep (longer than every pause) meant the Subscriber existed
# before the first sample. The 2026-09-25 re-sweep's "lost 0, LIFESPAN regressed" was that stagger,
# and so was the bisect built on it.
#
# HOW TO READ IT, written before running: TickLE should lose about (pause / 0.02) - 5, i.e. ~45 /
# 70 / 95 at pause 1.0 / 1.5 / 2.0. Those are the samples published during the pause minus the ~5
# still inside their 0.1 s lifespan, and it is what CycloneDDS loses. lost = 0 again would mean the
# stagger is still in effect (check the harness), not a core regression. Any other value is to be
# explained before it is published. The DDS twins run alongside as the control, with the same
# interleaving as the re-sweep.
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
OUT="${OUT:-/tmp/tickle_lifespan_recheck_$(date +%Y%m%d-%H%M%S).txt}"
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
ssh_h() { local h=$1; shift; ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$h" "$@"; }
git -C "$REPO" fetch -q origin
SHA="$(git -C "$REPO" rev-parse origin/main)"
say "=== lifespan recheck, $(date -Is), main $SHA, ${REPS} reps ==="
for h in "${HOSTS[@]}"; do
    ssh_h "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdq
cd examples/perf_hil && for fw in tickle cyclonedds fastdds; do (cd \$fw && ./build.sh lifespan_expiry >/tmp/lsrecheck_build.log 2>&1) || { echo BUILD FAILED \$fw; exit 1; }; done" &
done
wait
for pause in 1.0 1.5 2.0; do
    for rep in $(seq 1 "$REPS"); do
        for fw in tickle cyclonedds fastdds; do
            res=$(cd "$PH/$fw" && PRE_CLIENT_SLEEP=0 timeout 120 ./run_scenario.sh lifespan_expiry -i 0.02 -T 0.1 -n 250 -p "$pause" 2>/dev/null | grep '^RESULT:' | grep role=server | sed -E 's/RESULT: framework=[a-z]+ scenario=[a-z_]+ //' || true)
            say "pause $pause rep$rep $fw | ${res:-NO RESULT LINE}"
        done
    done
done
say "=== done $(date -Is) ==="
