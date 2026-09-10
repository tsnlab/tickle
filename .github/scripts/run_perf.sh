#!/usr/bin/env bash
# Hardware-in-the-loop performance test, run from the self-hosted runner (tickle-hil).
#
# Roles (fixed, per the two dedicated test Pis):
#   rpi#1 - client role (ping, perf_client)
#   rpi#2 - server role (pong, perf_server)
#
# Assumes:
#   - This runner already has an SSH key at ~/.ssh/tickle_ci_ed25519 authorized for
#     the "ci" account on both Pis (see .github/scripts/README.md for setup).
#   - Both Pis already have ~/tickle cloned (this script only fetches/checks out).
set -euo pipefail

RPI_CLIENT_HOST="${RPI_CLIENT_HOST:-10.1.1.207}" # rpi#1
RPI_SERVER_HOST="${RPI_SERVER_HOST:-10.1.1.213}" # rpi#2
SSH_USER="ci"
SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
REMOTE_DIR="tickle"

PING_COUNT="${PING_COUNT:-50}"
PING_INTERVAL="${PING_INTERVAL:-0.1}"
PERF_DURATION_SEC="${PERF_DURATION_SEC:-10}"
SMALL_MSG_SIZE="${SMALL_MSG_SIZE:-100}"

LOG_DIR="$(mktemp -d)"
trap 'rm -rf "$LOG_DIR"' EXIT

SSH_OPTS=(-i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

ssh_run() {
    local host="$1"
    shift
    # "$@" is a command line meant to run on $host - it is supposed to expand remotely.
    # shellcheck disable=SC2029
    ssh "${SSH_OPTS[@]}" "$SSH_USER@$host" "$@"
}

# Checks out the exact commit the runner itself is building, on both Pis, in parallel.
update_and_build() {
    local sha
    sha="$(git rev-parse HEAD)"
    echo "Checking out $sha and building on both Pis..."

    local pids=()
    for host in "$RPI_CLIENT_HOST" "$RPI_SERVER_HOST"; do
        ssh_run "$host" "
            set -e
            cd ~/$REMOTE_DIR
            git fetch --quiet origin
            git reset --hard --quiet $sha
            git clean -fdq
            make clean >/dev/null
            make all -j4
        " > "$LOG_DIR/build_$host.log" 2>&1 &
        pids+=("$!")
    done

    local failed=0
    for pid in "${pids[@]}"; do
        wait "$pid" || failed=1
    done
    if [ "$failed" -ne 0 ]; then
        echo "Build failed on at least one Pi:"
        for host in "$RPI_CLIENT_HOST" "$RPI_SERVER_HOST"; do
            echo "--- $host ---"
            cat "$LOG_DIR/build_$host.log"
        done
        exit 1
    fi
    echo "Build OK on both Pis."
}

# Starts the server binary in the background (with a generous -d safety cap in case the
# client hangs), runs the client in the foreground, then proactively stops the server
# instead of waiting out the safety cap.
#
# The example binaries build under platform/linux/ now (the native Linux build moved there when
# the repo split into platform/linux/ + platform/freertos/), not the repo root - update_and_build
# still runs `make all` from the root, which forwards there.
run_paired_test() {
    local label="$1" server_bin="$2" client_bin="$3" client_args="$4" server_safety_sec="$5"

    echo "== $label =="
    ssh_run "$RPI_SERVER_HOST" "cd ~/$REMOTE_DIR/platform/linux && ./$server_bin -d $server_safety_sec" \
        > "$LOG_DIR/${label}_server.log" 2>&1 &
    local server_pid=$!

    sleep 1 # let the server bind before the client starts sending

    ssh_run "$RPI_CLIENT_HOST" "cd ~/$REMOTE_DIR/platform/linux && ./$client_bin $client_args" \
        > "$LOG_DIR/${label}_client.log" 2>&1 || true

    # -x matches the exact process name, so this can't accidentally match its own
    # ssh invocation (which also contains the string "pong"/"perf_server" in argv).
    ssh_run "$RPI_SERVER_HOST" "pkill -INT -x $server_bin" || true
    wait "$server_pid" || true
}

summarize() {
    {
        echo "## Latency (ping / pong)"
        echo '```'
        cat "$LOG_DIR/latency_client.log"
        echo '```'
        echo
        echo "## Throughput (perf_client / perf_server)"
        echo "### Sender (rpi#1)"
        echo '```'
        cat "$LOG_DIR/throughput_client.log"
        echo '```'
        echo "### Receiver (rpi#2)"
        echo '```'
        cat "$LOG_DIR/throughput_server.log"
        echo '```'
        echo
        echo "## Small-message throughput ($SMALL_MSG_SIZE-byte payloads)"
        echo "### Sender (rpi#1)"
        echo '```'
        cat "$LOG_DIR/smallmsg_client.log"
        echo '```'
        echo "### Receiver (rpi#2)"
        echo '```'
        cat "$LOG_DIR/smallmsg_server.log"
        echo '```'
        local smsg_recv smsg_dur
        smsg_recv=$(grep -oP 'recv=\K[\d,]+' "$LOG_DIR/smallmsg_server.log" | tail -1 | tr -d ',')
        smsg_dur=$(grep -oP '[\d.]+(?= sec, avg)' "$LOG_DIR/smallmsg_server.log" | tail -1)
        if [ -n "${smsg_recv:-}" ] && [ -n "${smsg_dur:-}" ]; then
            echo
            echo "small-message rate: $(awk "BEGIN{printf \"%.0f\", $smsg_recv/$smsg_dur}") msg/sec"
        fi
    } | tee -a "${GITHUB_STEP_SUMMARY:-/dev/stdout}"
}

# Pulls the numbers back out of the example binaries' own stdout and writes them as
# github-action-benchmark's "custom" JSON format, so a later step can hand them
# straight to that action without this script knowing anything about benchmark storage.
write_benchmark_json() {
    local rtt_avg rtt_mdev loss_pct send_mbps recv_mbps

    rtt_avg=$(grep -oP 'rtt min/avg/max/mdev = [\d.]+/\K[\d.]+' "$LOG_DIR/latency_client.log")
    rtt_mdev=$(grep -oP 'rtt min/avg/max/mdev = [\d.]+/[\d.]+/[\d.]+/\K[\d.]+' "$LOG_DIR/latency_client.log")
    loss_pct=$(grep -oP '\d+(?=% packet loss)' "$LOG_DIR/latency_client.log")
    # perf_client.c/perf_server.c's "avg X Mbps" is comma-grouped past 999 (e.g. "avg 2,037.577
    # Mbps") - [\d,.]+ captures that, and tr strips the commas back out since a bare comma inside a
    # JSON number below would make it invalid JSON, not just a formatting choice.
    send_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/throughput_client.log" | tr -d ',')
    recv_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/throughput_server.log" | tr -d ',')

    cat > latency-benchmark.json <<EOF
[
  {"name": "rtt avg", "unit": "ms", "value": $rtt_avg},
  {"name": "rtt mdev", "unit": "ms", "value": $rtt_mdev},
  {"name": "packet loss", "unit": "%", "value": $loss_pct}
]
EOF

    cat > throughput-benchmark.json <<EOF
[
  {"name": "send throughput", "unit": "Mbps", "value": $send_mbps},
  {"name": "recv throughput", "unit": "Mbps", "value": $recv_mbps}
]
EOF
}

update_and_build

run_paired_test "latency" "pong" "ping" "-c $PING_COUNT -i $PING_INTERVAL" \
    "$(awk "BEGIN { printf \"%d\", ($PING_COUNT * $PING_INTERVAL) + 30 }")"

run_paired_test "throughput" "perf_server" "perf_client" "-d $PERF_DURATION_SEC" \
    "$((PERF_DURATION_SEC + 30))"

# Small-message run: 100-byte payloads that node_flush() batches several per packet, so this is
# limited by per-message CPU work (encode/decode/lookup/callback) rather than link bandwidth -
# the regime where internal optimizations show up as message rate even when a full-MTU run is
# already at line rate. Reported as messages/sec (its Mbps is mostly framing overhead).
run_paired_test "smallmsg" "perf_server" "perf_client" "-s $SMALL_MSG_SIZE -d $PERF_DURATION_SEC" \
    "$((PERF_DURATION_SEC + 30))"

summarize
write_benchmark_json
