#!/usr/bin/env bash
# Orchestrates one TickLE-native HIL scenario across the two rpis (rpi#1=client, rpi#2=server),
# mirroring ../cyclonedds/run_scenario.sh's own pattern (same '</dev/null'+';' fix for the SSH
# backgrounding hang, same $CLIENT_ARGS forwarded to both sides). No LD_LIBRARY_PATH/vendor URI
# needed - TickLE links statically (libtickle.a via pkg-config, build.sh) and has no discovery
# config knob equivalent to CycloneDDS's SPDPInterval.
set -euo pipefail

SCENARIO="${1:?usage: run_scenario.sh <scenario> [client_args...]}"
shift
CLIENT_ARGS="${*:--d 10}"

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
sleep 3
ssh_run "$RPI_CLIENT" "cd ~/$REMOTE_DIR && ./client $CLIENT_ARGS" | grep '^RESULT:'
sleep 1
ssh_run "$RPI_SERVER" "cat /tmp/tickle_${SCENARIO}_server.log" | grep '^RESULT:'
ssh_run "$RPI_SERVER" "pkill -INT -x server" || true
