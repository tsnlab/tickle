#!/usr/bin/env bash
# Where did RELIABLE+KEEP_ALL throughput under heavy loss go? COMPARISON.MD §3b publishes 89.3 Mbps
# at tc 20%; HEAD measures ~55. Already excluded, each by measurement rather than by argument:
#   - the same-host socket work (82a6a02d/2d72282c): ABBA A/B, 4 blocks, arms agree at 55.0
#   - 835e8b44's logging: "Publisher peer registered" fires exactly ONCE in a 20%-loss run
#   - run duration: -d5 gives 54.5 and -d10 gives 54.9, against a published method of 10s runs
#
# So the cause is earlier than 0be8c33f. This walks candidate core commits between the KEEP_ALL
# solicitation fix (a4378596, which §3b's own matrix was taken after) and 0be8c33f.
#
# The harness is held at HEAD on every arm and only src/ and include/ are moved. Older harnesses
# predate 9f70d9b4's `taskset -c 1-3`, and an unpinned sender can share a core with the NIC
# interrupt for a ~15% effect - so checking out a whole old tree would mix a harness difference
# into a core comparison. That is the mistake this experiment exists downstream of.
#
# How to read it: a step between adjacent shas is the commit that did it. No step anywhere means
# the cause is outside this window and the window was reconstructed wrongly.
set -euo pipefail
K="$HOME/.ssh/tickle_ci_ed25519"
CLIENT=10.1.1.214; SERVER=10.1.1.213
HEADSHA="$(git rev-parse HEAD)"
SHAS="${SHAS:-a4378596 b332dc1b 7bb87702 0be8c33f $HEADSHA}"
REPS="${REPS:-3}"
OUT="${OUT:-/tmp/tickle_loss_bisect.txt}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

sl() { if [ "$1" = 0 ]; then ssh -i "$K" -o BatchMode=yes "ci@$CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true
       else ssh -i "$K" -o BatchMode=yes "ci@$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss $1%"; fi; }
trap 'sl 0' EXIT

: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== RELIABLE loss bisect, harness pinned at HEAD, $(date -Is) ==="

for sha in $SHAS; do
    ok=1
    for host in "$CLIENT" "$SERVER"; do
        ssh -i "$K" -o BatchMode=yes "ci@$host" "set -e
cd ~/tickle
git fetch --quiet origin
git reset --hard --quiet $HEADSHA
git clean -fdq
git checkout --quiet $sha -- src include
rm -rf ~/tickle_local_install
cd ~/tickle/examples/perf_hil/tickle && ./build.sh reliable_throughput >/tmp/bisect_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/bisect_build.log; exit 1; }" || ok=0
    done
    if [ "$ok" != 1 ]; then say ""; say "--- $sha: BUILD FAILED against HEAD's harness, skipped ---"; continue; fi
    say ""
    say "--- core at $sha (harness at HEAD) ---"
    sl 20
    for rep in $(seq 1 "$REPS"); do
        line="$("$HERE/../tickle/run_scenario.sh" reliable_throughput -Q -d 5 2>/dev/null | grep '^RESULT:' | grep 'role=client' || true)"
        [ -n "$line" ] || { say "rep $rep: NO RESULT"; continue; }
        say "rep $rep: mbps=$(echo "$line" | grep -oP 'send_mbps=\K[0-9.]+') sent=$(echo "$line" | grep -oP ' sent=\K[0-9]+')"
    done
    sl 0
done

for host in "$CLIENT" "$SERVER"; do
    ssh -i "$K" -o BatchMode=yes "ci@$host" "cd ~/tickle && git reset --hard --quiet $HEADSHA && git clean -fdq" || true
done
say ""
say "=== done $(date -Is); tc restored, rig left at HEAD ==="
