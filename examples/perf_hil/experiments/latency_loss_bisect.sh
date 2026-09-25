#!/usr/bin/env bash
# Why does TickLE's latency scenario lose ~53 of 100 pings at -i 0.1 -d 10 (2026-09-24 re-sweep at
# 659013e9, 6/6 runs, BEST_EFFORT and RELIABLE alike, both DDS twins 0-1% in the same session)?
#
# Two hypotheses, told apart by duration, then located by commit:
#   startup  - a fixed ~5.3 s of pings goes out before the pair matches. Loss then falls with
#              duration: about 100% at -d 5, 53% at -d 10, 26% at -d 20.
#   pattern  - something drops a share of pings throughout (every other one, say). Loss then stays
#              near 50% whatever the duration.
# HOW TO READ IT, written before running: duration arm first. Then the same -i 0.1 -d 10 cell at
# each commit, oldest first. The first commit whose loss jumps is the one to look at, provided its
# predecessor is clean. If d6d312cd, from before today's core changes, already loses, then the loss
# predates today and the published 0% was taken at a different rate (the harness default, 1 Hz),
# so the 1 Hz arm is the control.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PH/../.." && pwd)"
if [ "${RIG_LOCK_HELD_HIL:-${RIG_LOCK_HELD:-0}}" != "1" ]; then
    exec "$PH/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
HOSTS=(10.1.1.214 10.1.1.213)
OUT="${OUT:-/tmp/tickle_latency_bisect_$(date +%Y%m%d-%H%M%S).txt}"
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
ssh_h() { local h=$1; shift; ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$h" "$@"; }

deploy() { # $1 sha
    local h
    for h in "${HOSTS[@]}"; do
        ssh_h "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $1 && git clean -fdqx
cd examples/perf_hil/tickle && ./build.sh best_effort_latency >/tmp/bisect_build.log 2>&1 || { echo BUILD FAILED; tail -5 /tmp/bisect_build.log; exit 1; }" &
    done
    wait
}
run() { # label, args...
    local label=$1; shift
    local res
    res=$(cd "$PH/tickle" && timeout 120 ./run_scenario.sh best_effort_latency "$@" 2>/dev/null | grep '^RESULT:' | grep -v role=server | sed -E 's/RESULT: framework=[a-z]+ scenario=[a-z_]+ //' || true)
    say "$label | ${res:-NO RESULT LINE}"
}

MAIN="$(git -C "$REPO" rev-parse origin/main)"
say "=== latency loss bisect, $(date -Is), main $MAIN ==="

say "--- duration and rate arms at 659013e9 ---"
deploy 659013e9
for d in 5 10 20; do run "659013e9 -i 0.1 -d $d" -i 0.1 -d "$d"; done
run "659013e9 -i 1.0 -d 10 (harness default rate)" -i 1.0 -d 10

say "--- the same -i 0.1 -d 10 cell by commit ---"
for sha in d6d312cd 6db149ab 477293da 82089b2f 718eaacf; do
    deploy "$sha"
    run "$sha -i 0.1 -d 10" -i 0.1 -d 10
    run "$sha -i 0.1 -d 10 (repeat)" -i 0.1 -d 10
done

deploy "$MAIN"
say "=== done $(date -Is); rpis left at main $MAIN ==="
