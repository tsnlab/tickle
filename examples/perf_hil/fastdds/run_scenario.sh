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
# Pins FastDDS to the eth0 test link, where tc applies - see fastdds_eth0_only.xml's header.
PROFILE="/home/ci/tickle/examples/perf_hil/fastdds/fastdds_eth0_only.xml" # path on the rpis (user ci, as in ssh_run)
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
#
# $CLIENT_ARGS forwarded to the server too, and a 5s (not 2s) pre-client sleep (2026-09-20, same
# real bug found and fixed on the CycloneDDS twin - see its own doc comment): durability_late_join
# needs -D on *both* sides (a durable client against a volatile server is a real RxO
# incompatibility, not a discovery bug) and a durable match negotiation takes measurably longer
# than a plain volatile one. Every other scenario's server.cpp only reads -d/-D and ignores
# anything else, so this is safe to do unconditionally, not just for that one scenario.
ssh_run "$RPI_SERVER" "export LD_LIBRARY_PATH=$LIB_PATH; export FASTRTPS_DEFAULT_PROFILES_FILE=$PROFILE; cd ~/$REMOTE_DIR; nohup ./server $CLIENT_ARGS > /tmp/fdds_${SCENARIO}_server.log 2>&1 < /dev/null &"
sleep 5
ssh_run "$RPI_CLIENT" "export LD_LIBRARY_PATH=$LIB_PATH; export FASTRTPS_DEFAULT_PROFILES_FILE=$PROFILE; cd ~/$REMOTE_DIR && ./client $CLIENT_ARGS" | grep '^RESULT:'
# pkill first, then read the log - not just pkill (2026-09-21, real gap found the hard way): this
# script never actually printed the server's own RESULT line at all before this fix - only the
# client's own line ever reached stdout, silently losing every server-side recv/loss number for
# every scenario whose server is the authoritative side (best_effort_throughput,
# reliable_throughput, ...) unless someone happened to read the remote log file by hand
# afterward. Matches examples/perf_hil/tickle/run_scenario.sh's own identical fix.
ssh_run "$RPI_SERVER" "pkill -INT -x server" || true
sleep 1
ssh_run "$RPI_SERVER" "cat /tmp/fdds_${SCENARIO}_server.log" | grep '^RESULT:'
