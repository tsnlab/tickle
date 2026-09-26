#!/usr/bin/env bash
# L2 of rmw_tickle/LIVELINESS_PLAN.md (2026-09-26): LIVELINESS detection on the rig, TickLE before and after the
# change, FastDDS and CycloneDDS, in one session. The publisher (client) is killed with SIGKILL mid-stream; the
# subscriber (server) reports detect_latency_ms, measured from the last sample it received, as all three
# harnesses do. Lease 1.0 / 2.0 / 4.0 s, REPS repetitions (default 20), the four arms interleaved in every rep.
#
# HOW TO READ IT (LIVELINESS_PLAN section 4, written before implementation):
#   PASS for the after arm if, for every lease, median(detect - lease) is within 20 ms, no rep is more than 50 ms
#   from that median, and 4.0 s is not cut to ~3 s.
#   CONTROL: the before arm must reproduce today's figures - a bimodal residual (a ~15 ms mode and a minority
#   ~500 ms away) and lease 4.0 s detected at ~3-3.6 s. If it does not, the harness has changed and the after
#   arm is not read.
#   The DDS arms are the reference the TickLE column is compared against (COMPARISON row 46).
#
# Processes are identified by the PID their own launch printed ($!), checked through /proc/PID/exe before any
# signal - never by name (CLAUDE.md rule 2).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
BEFORE=${BEFORE:?BEFORE=<sha> of the core before the change}
AFTER=${AFTER:?AFTER=<sha> of the change}
REPS=${REPS:-20}
OUT=${OUT:-/tmp/liveliness_l2.txt}
sh_() { local h=$1; shift; ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$h" "$@"; }
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
BEFORE=$(git -C "$REPO" rev-parse "$BEFORE"); AFTER=$(git -C "$REPO" rev-parse "$AFTER")
say "=== liveliness L2, $(date -Is), before ${BEFORE:0:8}, after ${AFTER:0:8}, $REPS reps ==="

# Builds: TickLE at BEFORE, kept aside in ~/l2_tickle_before; then everything at AFTER (the DDS harnesses are the
# same at both), TickLE's kept aside in ~/l2_tickle_after.
pids=()
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin
build() { (cd ~/tickle/examples/perf_hil/\$1 && ./build.sh liveliness_loss_detection p1 > /tmp/l2_build_\$1.log 2>&1) || { echo \"BUILD FAILED: \$1 at \$(git -C ~/tickle rev-parse --short HEAD)\"; tail -5 /tmp/l2_build_\$1.log; exit 1; }; }
git reset -q --hard $BEFORE && git clean -fdqx -e install -e build -e log
build tickle
rm -rf ~/l2_tickle_before && cp -r examples/perf_hil/tickle/liveliness_loss_detection_p1 ~/l2_tickle_before
git reset -q --hard $AFTER && git clean -fdqx -e install -e build -e log
build tickle; build cyclonedds; build fastdds
rm -rf ~/l2_tickle_after && cp -r examples/perf_hil/tickle/liveliness_loss_detection_p1 ~/l2_tickle_after
echo \"built on \$(hostname)\"" 2>&1 | tee -a "$OUT" &
    pids+=($!)
done
bad=0; for p in "${pids[@]}"; do wait "$p" || bad=1; done
[ "$bad" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }

dir_for() { case "$1" in
    tickle_before) echo "\$HOME/l2_tickle_before" ;;
    tickle_after) echo "\$HOME/l2_tickle_after" ;;
    *) echo "\$HOME/tickle/examples/perf_hil/$1/liveliness_loss_detection_p1" ;;
    esac; }
lease_flag() { case "$1" in tickle_*) echo "-T $2" ;; *) echo "-L $2" ;; esac; }

one() { # $1 arm, $2 lease, $3 rep
    local arm=$1 lease=$2 rep=$3 d fl spid cpid res verdict=ok
    d=$(dir_for "$arm"); fl=$(lease_flag "$arm" "$lease")
    spid=$(sh_ "$SERVER" "cd $d && rm -f /tmp/l2_server.log && nohup taskset -c 1-3 ./server $fl > /tmp/l2_server.log 2>&1 < /dev/null & echo \$!")
    cpid=$(sh_ "$CLIENT" "cd $d && nohup taskset -c 1-3 ./client $fl > /tmp/l2_client.log 2>&1 < /dev/null & echo \$!")
    sleep 6
    sh_ "$CLIENT" "[ \"\$(readlink /proc/$cpid/exe)\" = \"\$(readlink -f $d/client)\" ] && kill -9 $cpid" || verdict="VOID(client not found to kill)"
    local waited=0 limit
    limit=$(awk -v l="$lease" 'BEGIN{print int((l + 12) * 2)}') # half-seconds: the lease plus the node limit and margin
    until sh_ "$SERVER" "grep -q '^RESULT:' /tmp/l2_server.log" 2>/dev/null || [ "$waited" -ge "$limit" ]; do sleep 0.5; waited=$((waited + 1)); done
    sh_ "$SERVER" "[ -d /proc/$spid ] && [ \"\$(readlink /proc/$spid/exe)\" = \"\$(readlink -f $d/server)\" ] && kill -INT $spid; sleep 1; [ -d /proc/$spid ] && [ \"\$(readlink /proc/$spid/exe)\" = \"\$(readlink -f $d/server)\" ] && kill -9 $spid" > /dev/null 2>&1 || true
    res=$(sh_ "$SERVER" "grep '^RESULT:' /tmp/l2_server.log" | tr '\n' ' ')
    [ -n "$res" ] || { [ "$verdict" = ok ] && verdict="VOID(no RESULT)"; }
    say "$arm lease=$lease rep$rep | $verdict | ${res#RESULT: }"
    sleep 1
}

for rep in $(seq 1 "$REPS"); do
    for lease in 1.0 2.0 4.0; do
        for arm in tickle_before tickle_after cyclonedds fastdds; do
            one "$arm" "$lease" "$rep"
        done
    done
done
say "=== done ==="
