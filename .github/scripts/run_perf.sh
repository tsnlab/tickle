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

RPI_CLIENT_HOST="${RPI_CLIENT_HOST:-10.1.1.214}" # rpi#1 (was .207 until 2026-09; DHCP reassigned)
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

# Fragment the "Performance Test" workflow hands to .github/scripts/publish_dashboard.sh for the
# Raspberry Pi row of https://tsnlab.github.io/tickle/dev/bench/. Seed it as a failure now and
# rewrite it once results exist, so an early abort (build failure, SSH timeout) still leaves the
# dashboard an honest red.
FRAG="${DASHBOARD_FRAGMENT:-perf-frag.json}"
printf '{"build":"fail","integration":"fail","commit":"%s","commit_short":"%s","date":"%s"}\n' \
    "$(git rev-parse HEAD)" "$(git rev-parse --short HEAD)" "$(date -u +%Y-%m-%dT%H:%MZ)" > "$FRAG"

write_dashboard_fragment() {
    local rtt_avg rtt_mdev loss_pct send_mbps recv_mbps integ smsg_rate smsg_recv smsg_dur

    rtt_avg=$(grep -oP 'rtt min/avg/max/mdev = [\d.]+/\K[\d.]+' "$LOG_DIR/latency_client.log" || true)
    rtt_mdev=$(grep -oP 'rtt min/avg/max/mdev = [\d.]+/[\d.]+/[\d.]+/\K[\d.]+' "$LOG_DIR/latency_client.log" || true)
    loss_pct=$(grep -oP '\d+(?=% packet loss)' "$LOG_DIR/latency_client.log" || true)
    send_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/throughput_client.log" | tr -d ',' || true)
    recv_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/throughput_server.log" | tr -d ',' || true)
    smsg_recv=$(grep -oP 'recv=\K[\d,]+' "$LOG_DIR/smallmsg_server.log" | tail -1 | tr -d ',' || true)
    smsg_dur=$(grep -oP '[\d.]+(?= sec, avg)' "$LOG_DIR/smallmsg_server.log" | tail -1 || true)

    # A round trip happened at all (ping got replies, throughput parsed) => integration pass.
    integ=fail
    if [ -n "${loss_pct:-}" ] && [ "${loss_pct}" -lt 100 ] && [ -n "${send_mbps:-}" ]; then
        integ=pass
    fi
    smsg_rate=null
    if [ -n "${smsg_recv:-}" ] && [ -n "${smsg_dur:-}" ]; then
        smsg_rate=$(awk "BEGIN{printf \"%.0f\", $smsg_recv/$smsg_dur}")
    fi

    cat > "$FRAG" <<EOF
{
  "build": "pass",
  "integration": "$integ",
  "commit": "$(git rev-parse HEAD)",
  "commit_short": "$(git rev-parse --short HEAD)",
  "date": "$(date -u +%Y-%m-%dT%H:%MZ)",
  "run_url": "${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-tsnlab/tickle}/actions/runs/${GITHUB_RUN_ID:-0}",
  "throughput_send_mbps": ${send_mbps:-null},
  "throughput_recv_mbps": ${recv_mbps:-null},
  "rtt_avg_ms": ${rtt_avg:-null},
  "rtt_mdev_ms": ${rtt_mdev:-null},
  "loss_pct": ${loss_pct:-null},
  "smallmsg_rate_msgs_s": ${smsg_rate}
}
EOF
}

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
    local label="$1" server_bin="$2" client_bin="$3" client_args="$4" server_safety_sec="$5" server_args="${6:-}"

    echo "== $label =="
    ssh_run "$RPI_SERVER_HOST" "cd ~/$REMOTE_DIR/platform/linux && ./$server_bin -d $server_safety_sec $server_args" \
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
        echo "## RELIABLE throughput (QoS roadmap #5, rmw_tickle/PLAN.md)"
        echo "### Sender (rpi#1)"
        echo '```'
        cat "$LOG_DIR/reliable_client.log"
        echo '```'
        echo "### Receiver (rpi#2)"
        echo '```'
        cat "$LOG_DIR/reliable_server.log"
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
    local rtt_avg rtt_mdev loss_pct send_mbps recv_mbps reliable_send_mbps reliable_recv_mbps reliable_loss_pct

    rtt_avg=$(grep -oP 'rtt min/avg/max/mdev = [\d.]+/\K[\d.]+' "$LOG_DIR/latency_client.log")
    rtt_mdev=$(grep -oP 'rtt min/avg/max/mdev = [\d.]+/[\d.]+/[\d.]+/\K[\d.]+' "$LOG_DIR/latency_client.log")
    loss_pct=$(grep -oP '\d+(?=% packet loss)' "$LOG_DIR/latency_client.log")
    # perf_client.c/perf_server.c's "avg X Mbps" is comma-grouped past 999 (e.g. "avg 2,037.577
    # Mbps") - [\d,.]+ captures that, and tr strips the commas back out since a bare comma inside a
    # JSON number below would make it invalid JSON, not just a formatting choice.
    send_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/throughput_client.log" | tr -d ',')
    recv_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/throughput_server.log" | tr -d ',')
    reliable_send_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/reliable_client.log" | tr -d ',')
    reliable_recv_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/reliable_server.log" | tr -d ',')
    # perf_server.c's "RESULT: ... loss_pct=X.X" - see run_paired_test's own "reliable" call
    # comment on why this isn't a clean recovered-vs-lost measurement yet.
    reliable_loss_pct=$(grep -oP 'loss_pct=\K[\d.]+' "$LOG_DIR/reliable_server.log" | tail -1)

    cat > latency-benchmark.json <<EOF
[
  {"name": "rtt avg", "unit": "ms", "value": $rtt_avg},
  {"name": "packet loss", "unit": "%", "value": $loss_pct}
]
EOF

    # rtt mdev (ping's own mean-deviation stat) is inherently noisy on real hardware in a way
    # avg/loss aren't - it alone tripped performance.yml's 200% alert three separate times in
    # one session, on commits nowhere near the ping/pong path. Tracked in its own benchmark
    # group (performance.yml's "Track latency jitter history", fail-on-alert: false) instead of
    # latency-benchmark.json above, so it still shows up in the history/graphs without being able
    # to fail the build on its own.
    cat > latency-jitter-benchmark.json <<EOF
[
  {"name": "rtt mdev", "unit": "ms", "value": $rtt_mdev}
]
EOF

    cat > throughput-benchmark.json <<EOF
[
  {"name": "send throughput", "unit": "Mbps", "value": $send_mbps},
  {"name": "recv throughput", "unit": "Mbps", "value": $recv_mbps}
]
EOF

    cat > reliable-throughput-benchmark.json <<EOF
[
  {"name": "reliable send throughput", "unit": "Mbps", "value": $reliable_send_mbps},
  {"name": "reliable recv throughput", "unit": "Mbps", "value": $reliable_recv_mbps}
]
EOF

    # Split from reliable-throughput-benchmark.json above for the same reason latency-jitter is
    # split from latency-benchmark.json: a different alert direction (smaller is better) - and,
    # since this number isn't a clean recovered-vs-lost measurement yet (see this file's own
    # "reliable" run_paired_test comment), it shouldn't be able to fail the build on its own either.
    cat > reliable-loss-benchmark.json <<EOF
[
  {"name": "reliable loss_pct", "unit": "%", "value": $reliable_loss_pct}
]
EOF
}

update_and_build

run_paired_test "latency" "pong" "ping" "-c $PING_COUNT -i $PING_INTERVAL" \
    "$(awk "BEGIN { printf \"%d\", ($PING_COUNT * $PING_INTERVAL) + 30 }")"

run_paired_test "throughput" "perf_server" "perf_client" "-d $PERF_DURATION_SEC" \
    "$((PERF_DURATION_SEC + 30))"

# RELIABLE run: QoS roadmap #5 (RELIABILITY/RELIABLE, rmw_tickle/PLAN.md) - same shape as the
# best-effort "throughput" run above, but both sides pass -R so perf_client's Publisher retains
# samples for retransmission and perf_server's Subscriber ACKNACKs on a gap. Tracks reliable
# delivery's own throughput cost against best-effort - NOT a clean loss/no-loss comparison:
# perf_server.c's own drop counter (bulk_callback()'s expected_seq check) predates this feature
# and isn't retransmission-aware, so a sample that *was* successfully recovered but arrived late
# (out of its original seq_no order) still counts as a gap there, same as a never-recovered one
# would. Read reliable-run throughput here as the real number; read its loss_pct as "how often
# reordering happened", not "how much data never arrived" - the latter would need perf_server.c
# itself taught to recognize a late, out-of-order arrival as a recovered duplicate rather than a
# fresh gap, left for whenever that distinction is actually needed.
run_paired_test "reliable" "perf_server" "perf_client" "-d $PERF_DURATION_SEC -R" \
    "$((PERF_DURATION_SEC + 30))" "-R"

# Small-message run: 100-byte payloads, -B so node_flush() batches several per packet (perf_
# client's own default flipped to flush-immediately, one packet per message - see DESIGN.md's
# "RPC and Publish flush immediately by default; batching is opt-in" - which would otherwise make
# this a link/syscall-bound measurement instead of the per-message-CPU-bound one it's meant to be).
# With batching restored, this is limited by per-message CPU work (encode/decode/lookup/callback)
# rather than link bandwidth - the regime where internal optimizations show up as message rate
# even when a full-MTU run is already at line rate. Reported as messages/sec (its Mbps is mostly
# framing overhead).
run_paired_test "smallmsg" "perf_server" "perf_client" "-s $SMALL_MSG_SIZE -d $PERF_DURATION_SEC -B" \
    "$((PERF_DURATION_SEC + 30))"

summarize
write_benchmark_json
write_dashboard_fragment
