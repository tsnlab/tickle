#!/usr/bin/env bash
# Mirrors cyclonedds/run_scenario.sh exactly, for the FastDDS side.
set -euo pipefail

SCENARIO="${1:?usage: run_scenario.sh <scenario> [client_args...]}"
shift
CLIENT_ARGS="${*:--i 0.1 -d 10}"

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT="10.1.1.214"
RPI_SERVER="10.1.1.213"
LIB_PATH="/opt/ros/jazzy/lib"
REMOTE_DIR="tickle/examples/perf_hil/fastdds/$SCENARIO"

ssh_run() {
    local host="$1"
    shift
    # shellcheck disable=SC2029
    ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=5 "ci@$host" "$@"
}

# ';' (not '&&') and </dev/null on the backgrounded remote process (2026-09-20, real hang found
# the hard way on the CycloneDDS twin of this script - see its own doc comment for the full
# repro): `cd dir && nohup cmd &` never returns control to a non-interactive ssh client at all,
# reproduced with a plain `nohup sleep 30 &`, nothing framework-specific about it.
ssh_run "$RPI_SERVER" "export LD_LIBRARY_PATH=$LIB_PATH; cd ~/$REMOTE_DIR; nohup ./server > /tmp/fdds_${SCENARIO}_server.log 2>&1 < /dev/null &"
sleep 2
ssh_run "$RPI_CLIENT" "export LD_LIBRARY_PATH=$LIB_PATH; cd ~/$REMOTE_DIR && ./client $CLIENT_ARGS" | grep '^RESULT:'
ssh_run "$RPI_SERVER" "pkill -INT -x server" || true
