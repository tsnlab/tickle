#!/usr/bin/env bash
# Orchestrates one TickLE-native HIL scenario across the two rpis (rpi#1=client, rpi#2=server),
# mirroring ../cyclonedds/run_scenario.sh's own pattern (same '</dev/null'+';' fix for the SSH
# backgrounding hang, same $CLIENT_ARGS forwarded to both sides). No LD_LIBRARY_PATH/vendor URI
# needed - TickLE links statically (libtickle.a via pkg-config, build.sh) and has no discovery
# config knob equivalent to CycloneDDS's SPDPInterval.
set -euo pipefail

# Rig mutual exclusion (examples/perf_hil/rig_lock.sh): re-exec under the lock unless an outer
# scope (a sweep, or CI's run_perf.sh) already holds it - see that script's own header for the
# CI-vs-manual collision this prevents.
if [ "${RIG_LOCK_HELD:-0}" != "1" ]; then
    exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi

SCENARIO="${1:?usage: run_scenario.sh <scenario> [client_args...]}"
shift
CLIENT_ARGS="${*:--d 10}"
# Overridable, not just for tuning (2026-09-21, real bug found the hard way): every other
# scenario's own server.c is a passive matcher (client can't usefully start until it exists), so a
# fixed pre-client sleep is a safe default everywhere - except history_depth_burst_loss, whose
# server.c does its OWN internal `-p` stall *before ever creating its Subscriber at all*. With the
# default 3s here, that scenario's subscriber was becoming discoverable (server's own internal
# pause completing) *before* the client had even started publishing (client only starts after this
# sleep, plus its own 2s discovery margin) - the eviction window under test never actually existed,
# silently producing 0 loss regardless of `-p`. `PRE_CLIENT_SLEEP=0` restores the real race: both
# sides launch together, and server.c's own `-p` is what creates the whole delay, same as the
# CycloneDDS/FastDDS twins achieve structurally via their own writer's match-wait blocking on a
# reader that doesn't yet exist - TickLE's own Publisher has no such match-wait to lean on (publish()
# is unconditional broadcast, verified directly in tickle.c), so this has to be an explicit,
# scenario-aware orchestration choice instead.
PRE_CLIENT_SLEEP="${PRE_CLIENT_SLEEP:-3}"

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT="10.1.1.214"
RPI_SERVER="10.1.1.213"
REMOTE_DIR="tickle/examples/perf_hil/tickle/$SCENARIO"

ssh_run() {
    local host="$1"
    shift
    # shellcheck disable=SC2029
    ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=5 "ci@$host" "$@"
}

ssh_run "$RPI_SERVER" "cd ~/$REMOTE_DIR; nohup ./server $CLIENT_ARGS > /tmp/tickle_${SCENARIO}_server.log 2>&1 < /dev/null &"
sleep "$PRE_CLIENT_SLEEP"
ssh_run "$RPI_CLIENT" "cd ~/$REMOTE_DIR && ./client $CLIENT_ARGS" | grep '^RESULT:'
# SIGINT the server right after the client finishes, then read its log - not just a fixed sleep
# (2026-09-21, real bug found the hard way): some scenarios' own server.c only prints its own
# RESULT line once interrupted or once its own (possibly much longer than the client's) internal
# deadline elapses - durability_late_join/server.c's own `-d` (ack wait, default 40s) is the clear
# example. A bare `sleep 1; cat log` raced that and found no RESULT line yet, and `pipefail` then
# aborted the whole script (the real symptom this fixes) before ever reaching the pkill below.
ssh_run "$RPI_SERVER" "pkill -INT -x server" || true
sleep 1
ssh_run "$RPI_SERVER" "cat /tmp/tickle_${SCENARIO}_server.log" | grep '^RESULT:'
