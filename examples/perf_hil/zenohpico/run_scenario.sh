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
# The reliable scenario is TCP peer-to-peer with no router - the subscriber listens, the publisher connects - because
# that is the only configuration where zenoh-pico's reliability is real (ZENOH_PICO_PLAN.md section 1). The best-effort
# scenario stays on UDP multicast, where both sides make the same best-effort promise. The endpoints are the rig's
# measurement link, not the management LAN.
if [ "${SCEN_DIR%%_*}" = reliable ]; then
    ZENOH_SERVER_ADDR="${ZENOH_SERVER_ADDR:-192.168.10.2}"
    ZENOH_EP_LISTEN="BENCH_ZENOH_LISTEN=tcp/$ZENOH_SERVER_ADDR:7447"
    ZENOH_EP_CONNECT="BENCH_ZENOH_CONNECT=tcp/$ZENOH_SERVER_ADDR:7447"
else
    ZENOH_EP_LISTEN=""
    ZENOH_EP_CONNECT=""
fi
ssh_run "$RPI_SERVER" "cd ~/$REMOTE_DIR; nohup env $ZENOH_EP_LISTEN $PIN stdbuf -oL ./server $* > /tmp/zenohpico_server.log 2>&1 < /dev/null &"
sleep 2
ssh_run "$RPI_CLIENT" "cd ~/$REMOTE_DIR && env $ZENOH_EP_CONNECT $PIN stdbuf -oL ./client $*" | grep '^RESULT:'
sleep 2
ssh_run "$RPI_SERVER" "pkill -INT -x server" || true
sleep 1
ssh_run "$RPI_SERVER" "cat /tmp/zenohpico_server.log" | grep '^RESULT:'
