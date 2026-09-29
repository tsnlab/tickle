#!/usr/bin/env bash
# run_scenario.sh <scenario>_<payload> [args] - the zenoh-pico twin of the other three frameworks' runner, so
# campaign_sweep.sh drives all four the same way. Hosts are overridable for a same-host cell, as in the others.
set -euo pipefail
SCEN_DIR=${1:?usage: run_scenario.sh <scenario>_<payload> [args]}
shift || true
SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT="${RPI_CLIENT:-10.1.1.214}"
RPI_SERVER="${RPI_SERVER:-10.1.1.213}"
REMOTE_DIR="tickle/examples/perf_hil/zenohpico/$SCEN_DIR"
PIN=${PIN:-taskset -c 1-3}
ssh_run() {
    local host=$1
    shift
    ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=5 "ci@$host" "$@"
}
# stdbuf: zenoh-pico's examples and this harness print through libc, and with stdout redirected the RESULT line
# sits in a full buffer until exit - a subscriber killed before it flushes looks exactly like one that received
# nothing. Found the first time this harness was run (2026-09-29).
ssh_run "$RPI_SERVER" "cd ~/$REMOTE_DIR; nohup $PIN stdbuf -oL ./server $* > /tmp/zenohpico_server.log 2>&1 < /dev/null &"
sleep 2
ssh_run "$RPI_CLIENT" "cd ~/$REMOTE_DIR && $PIN stdbuf -oL ./client $*" | grep '^RESULT:'
sleep 2
ssh_run "$RPI_SERVER" "pkill -INT -x server" || true
sleep 1
ssh_run "$RPI_SERVER" "cat /tmp/zenohpico_server.log" | grep '^RESULT:'
