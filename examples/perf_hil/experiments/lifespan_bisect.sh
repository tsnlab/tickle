#!/usr/bin/env bash
# TickLE's lifespan_expiry now loses 0 samples at every pause (2026-09-24 re-sweep at 659013e9,
# 9/9 runs: recv=250 lost=0 at pause 1.0/1.5/2.0 s). The published figures were lost 28/43/79,
# and both DDS twins in the same session lost 39-95, as they should. Samples that outlived their
# 100 ms lifespan are being delivered, i.e. LIFESPAN is not enforced any more. This finds when.
#
# Method: the lifespan scenario at pause 1.0 s (run_perf.sh's args), one run per commit. The
# scenario is deterministic here - every DDS rep matched exactly and every TickLE rep read 0.
# Each commit's own harness is used, since the lifespan harness has existed since 2026-09-20.
# HOW TO READ IT, written before running: lost > 0 means LIFESPAN is enforced at that commit, and
# lost = 0 means it is not. The first commit reading 0 after one reading > 0 introduced it. If the
# oldest commit already reads 0, the published figures were taken some other way, and the harness
# is what to look at.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PH/../.." && pwd)"
if [ "${RIG_LOCK_HELD_HIL:-${RIG_LOCK_HELD:-0}}" != "1" ]; then
    exec "$PH/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi
SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
HOSTS=(10.1.1.214 10.1.1.213)
SHAS="${SHAS:-d7ac3ee5 8634aaa0 fa3265f0 261f39b8 d63860c7 7cc9fd45 d6d312cd 82089b2f 659013e9}"
OUT="${OUT:-/tmp/tickle_lifespan_bisect_$(date +%Y%m%d-%H%M%S).txt}"
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
ssh_h() { local h=$1; shift; ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$h" "$@"; }
deploy() {
    local h
    for h in "${HOSTS[@]}"; do
        ssh_h "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $1 && git clean -fdqx
cd examples/perf_hil/tickle && ./build.sh lifespan_expiry >/tmp/lsbisect_build.log 2>&1 || { echo BUILD FAILED; tail -5 /tmp/lsbisect_build.log; exit 1; }" &
    done
    wait
}
git -C "$REPO" fetch -q origin
MAIN="$(git -C "$REPO" rev-parse origin/main)"
say "=== lifespan bisect, $(date -Is), commits: $SHAS ==="
for sha in $SHAS; do
    if ! deploy "$sha"; then say "$sha | BUILD FAILED"; continue; fi
    res=$(cd "$PH/tickle" && timeout 120 ./run_scenario.sh lifespan_expiry -i 0.02 -T 0.1 -n 250 -p 1.0 2>/dev/null | grep '^RESULT:' | grep role=server | sed -E 's/RESULT: framework=[a-z]+ scenario=[a-z_]+ //' || true)
    say "$sha $(git -C "$REPO" log -1 --format=%ad --date=short "$sha") | ${res:-NO RESULT LINE}"
done
deploy "$MAIN"
say "=== done $(date -Is); rpis left at main $MAIN ==="
