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

# RPI_CLIENT_HOST/RPI_SERVER_HOST above are the Pis' *management* addresses (this script's own
# SSH target) - a separate network from the dedicated point-to-point link the actual TickLE test
# traffic runs over, which every example binary this script runs defaults its own `-b` broadcast
# address to (examples/linux/common/cli_opts.c) and this script never overrides. probe_loss_
# testing() below needs *that* link's own interface on rpi#1, not whichever one happens to route
# toward RPI_SERVER_HOST's management IP (that could be a shared Wi-Fi/LAN uplink instead).
PERF_LINK_BROADCAST="${PERF_LINK_BROADCAST:-192.168.10.255}"

PING_COUNT="${PING_COUNT:-50}"
PING_INTERVAL="${PING_INTERVAL:-0.1}"
PERF_DURATION_SEC="${PERF_DURATION_SEC:-10}"
SMALL_MSG_SIZE="${SMALL_MSG_SIZE:-100}"
# The loss-injection scenarios' own send interval - deliberately *not* PERF_DURATION_SEC's own
# "-i 0" (as fast as poll() allows) default the clean-link throughput/reliable runs use. At a
# firehose send rate, tt_MAX_RELIABLE_HISTORY (8 samples, config.h) gets overwritten many times
# over before an ACKNACK's round trip can ever come back, so a NACKed sample is usually already
# evicted by the time it's requested - RELIABLE's own retransmission never gets a real chance to
# recover anything, and its ACKNACK/retransmit traffic just adds load to an already-lossy link on
# top of that. 20ms comfortably covers a real round trip on this rig (avg one-way latency here is
# a few ms - perf_server.c's own avg_latency_ms) so a NACKed sample should still be in cache when
# the retry arrives.
LOSS_TEST_INTERVAL_SEC="${LOSS_TEST_INTERVAL_SEC:-0.02}"
# perf_server.c's own -W (cooldown): without this, run_paired_test's pkill -INT right when
# perf_client exits gave the server's own gap tracking (track_arrival()/finalize_gap_tracking())
# zero time to let a still-recovering RELIABLE gap near the very end of the run actually resolve -
# perf_client.c's matching RELIABLE_SHUTDOWN_GRACE_SEC keeps the *sending* side alive just as long,
# so a late ACKNACK for one of the last few samples still gets a real retransmit. Found the same
# way as the fix this comment sits next to: reliable's own loss_pct plateaued at the *same* value
# at two different tc loss levels, which a shutdown race explains far better than anything
# proportional to loss probability would.
LOSS_TEST_COOLDOWN_SEC="${LOSS_TEST_COOLDOWN_SEC:-1.5}"

LOG_DIR="$(mktemp -d)"

# QoS roadmap #5 (RELIABILITY/RELIABLE) loss-injection scenarios: how BEST_EFFORT vs RELIABLE
# actually behave under real packet loss, using Linux's own tc/netem on rpi#1's (the sender's)
# egress toward rpi#2 - nothing TickLE-side, this is purely a network-layer fault injection.
# Needs passwordless `sudo tc` on rpi#1 (see README.md's own "What's needed") - probed for below,
# degrades to skipping just this section (not the whole run) if that isn't set up yet.
LOSS_LEVELS_PCT="${LOSS_LEVELS_PCT:-1 5 10}"
CLIENT_IFACE=""          # resolved below; set_loss() below is always a safe no-op while empty
LOSS_TESTING_AVAILABLE=0 # 1 once both the interface and passwordless sudo tc are confirmed

# pct == 0 clears any active netem qdisc instead of applying one - the one function both this
# script's own loss-level loop and the exit trap below (a run that dies mid-loss-level must not
# leave the *next* run on this same rig - latency/throughput/etc - silently lossy) share.
set_loss() {
    local pct="$1"
    [ -z "$CLIENT_IFACE" ] && return 0
    if [ "$pct" = "0" ]; then
        ssh_run "$RPI_CLIENT_HOST" "sudo -n tc qdisc del dev $CLIENT_IFACE root" >/dev/null 2>&1 || true
    else
        ssh_run "$RPI_CLIENT_HOST" "sudo -n tc qdisc replace dev $CLIENT_IFACE root netem loss ${pct}%"
    fi
}

trap 'set_loss 0; rm -rf "$LOG_DIR"' EXIT

# Fragment the "Performance Test" workflow hands to .github/scripts/publish_dashboard.sh for the
# Raspberry Pi row of https://tsnlab.github.io/tickle/dev/bench/. Seed it as a failure now and
# rewrite it once results exist, so an early abort (build failure, SSH timeout) still leaves the
# dashboard an honest red.
FRAG="${DASHBOARD_FRAGMENT:-perf-frag.json}"
printf '{"build":"fail","integration":"fail","commit":"%s","commit_short":"%s","date":"%s"}\n' \
    "$(git rev-parse HEAD)" "$(git rev-parse --short HEAD)" "$(date -u +%Y-%m-%dT%H:%MZ)" > "$FRAG"

write_dashboard_fragment() {
    local rtt_avg rtt_mdev loss_pct send_mbps recv_mbps integ smsg_rate smsg_recv smsg_dur
    local reliable_1pct_mbps reliable_5pct_mbps reliable_10pct_mbps

    rtt_avg=$(grep -oP 'rtt min/avg/max/mdev = [\d.]+/\K[\d.]+' "$LOG_DIR/latency_client.log" || true)
    rtt_mdev=$(grep -oP 'rtt min/avg/max/mdev = [\d.]+/[\d.]+/[\d.]+/\K[\d.]+' "$LOG_DIR/latency_client.log" || true)
    loss_pct=$(grep -oP '\d+(?=% packet loss)' "$LOG_DIR/latency_client.log" || true)
    send_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/throughput_client.log" | tr -d ',' || true)
    recv_mbps=$(grep -oP 'avg \K[\d,.]+(?= Mbps)' "$LOG_DIR/throughput_server.log" | tr -d ',' || true)
    smsg_recv=$(grep -oP 'recv=\K[\d,]+' "$LOG_DIR/smallmsg_server.log" | tail -1 | tr -d ',' || true)
    smsg_dur=$(grep -oP '[\d.]+(?= sec, avg)' "$LOG_DIR/smallmsg_server.log" | tail -1 || true)
    # Status-table columns for the tc/netem loss-injection scenarios (see probe_loss_testing()'s
    # own comment on why these three specific files might not exist at all) - RELIABLE's own
    # recv-side throughput at each fixed loss level, matching LOSS_LEVELS_PCT's own "1 5 10"
    # default exactly (a change to that default needs matching field/column renames here and in
    # dashboard.py's _row()/render_block(), not handled generically on purpose - three fixed
    # columns are simpler than a dynamic-width table for a rig that's never actually changed this).
    reliable_1pct_mbps=$(grep -oP 'avg_mbps=\K[\d,.]+' "$LOG_DIR/loss1_reliable_server.log" 2>/dev/null | tail -1 | tr -d ',' || true)
    reliable_5pct_mbps=$(grep -oP 'avg_mbps=\K[\d,.]+' "$LOG_DIR/loss5_reliable_server.log" 2>/dev/null | tail -1 | tr -d ',' || true)
    reliable_10pct_mbps=$(grep -oP 'avg_mbps=\K[\d,.]+' "$LOG_DIR/loss10_reliable_server.log" 2>/dev/null | tail -1 | tr -d ',' || true)

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
  "smallmsg_rate_msgs_s": ${smsg_rate},
  "reliable_throughput_1pct_mbps": ${reliable_1pct_mbps:-null},
  "reliable_throughput_5pct_mbps": ${reliable_5pct_mbps:-null},
  "reliable_throughput_10pct_mbps": ${reliable_10pct_mbps:-null}
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

# Resolves rpi#1's own outgoing interface for the dedicated rpi#1<->rpi#2 test link (routing to
# PERF_LINK_BROADCAST, not RPI_SERVER_HOST - see that variable's own comment on why those two can
# differ) and confirms `sudo tc` actually works non-interactively there - `sudo -n` fails fast
# instead of hanging on a password prompt that can never be answered over a non-interactive SSH
# session. Sets CLIENT_IFACE/LOSS_TESTING_AVAILABLE; never fails the script itself (set -e-safe:
# every check here is the condition of an `if`), just leaves loss testing unavailable with a
# clear reason logged.
probe_loss_testing() {
    CLIENT_IFACE=$(ssh_run "$RPI_CLIENT_HOST" "ip route get $PERF_LINK_BROADCAST" 2>/dev/null |
        grep -oP 'dev \K\S+' | head -1 || true)
    if [ -z "$CLIENT_IFACE" ]; then
        echo "Could not resolve rpi#1's outgoing interface for $PERF_LINK_BROADCAST - skipping loss-injection scenarios" >&2
        return
    fi
    echo "rpi#1's test-link traffic (toward $PERF_LINK_BROADCAST) goes out $CLIENT_IFACE"

    if ssh_run "$RPI_CLIENT_HOST" "sudo -n tc qdisc replace dev $CLIENT_IFACE root netem loss 1%" >/dev/null 2>&1; then
        set_loss 0
        LOSS_TESTING_AVAILABLE=1
    else
        CLIENT_IFACE="" # also disarms set_loss()'s own exit-trap cleanup - nothing was ever applied
        echo "rpi#1 cannot run 'sudo tc' non-interactively - skipping loss-injection scenarios" \
            "(see .github/scripts/README.md's own sudoers requirement)" >&2
    fi
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
        if [ "$LOSS_TESTING_AVAILABLE" = "1" ]; then
            echo "## RELIABLE vs BEST_EFFORT under packet loss (tc/netem, rpi#1's own egress)"
            echo
            echo "One-way latency is the receiver's clock minus the sender's own wire timestamp -"
            echo "only as accurate as the two Pis' clock sync (NTP); read it as a same-rig relative"
            echo "comparison across rows, not an absolute number. loss_pct is perf_server's own"
            echo "expected_seq gap counter, which (see the \"reliable\" run's own comment above)"
            echo "still counts a successfully-recovered-but-reordered RELIABLE sample as a gap."
            echo
            echo "| tc loss | mode | throughput (Mbps) | avg latency (ms) | loss_pct |"
            echo "|---|---|---|---|---|"
            for pct in $LOSS_LEVELS_PCT; do
                for mode in besteffort reliable; do
                    local log="$LOG_DIR/loss${pct}_${mode}_server.log"
                    local mbps lat lp
                    mbps=$(grep -oP 'avg_mbps=\K[\d,.]+' "$log" 2>/dev/null | tail -1 | tr -d ',' || true)
                    lat=$(grep -oP 'avg_latency_ms=\K[\d.]+' "$log" 2>/dev/null | tail -1 || true)
                    lp=$(grep -oP 'loss_pct=\K[\d.]+' "$log" 2>/dev/null | tail -1 || true)
                    echo "| ${pct}% | $mode | ${mbps:-N/A} | ${lat:-N/A} | ${lp:-N/A} |"
                done
            done
            echo
            # TEMPORARY (QoS roadmap #5 loss-injection investigation): raw sender (Publisher) log
            # for the worst-case level, including process_acknack()'s own per-retransmit
            # diagnostics (tickle.c) - remove once RELIABLE's own loss_pct is understood/fixed.
            echo "### DEBUG: rpi#1 (Publisher) log, worst loss level"
            echo '```'
            tail -c 100000 "$LOG_DIR/loss${LOSS_LEVELS_PCT##* }_reliable_client.log" 2>/dev/null || true
            echo '```'
            echo
            # TEMPORARY (QoS roadmap #5 loss-injection investigation): same, but the receiver
            # (Subscriber) side - acknack_retry()'s own give-up log and skip_unrecoverable_
            # backlog()'s own diagnostic (tickle.c) only ever run here, not on the Publisher side
            # dumped above.
            echo "### DEBUG: rpi#2 (Subscriber) log, worst loss level"
            echo '```'
            tail -c 100000 "$LOG_DIR/loss${LOSS_LEVELS_PCT##* }_reliable_server.log" 2>/dev/null || true
            echo '```'
            echo
        fi
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

    # tc/netem loss-injection scenarios (see probe_loss_testing()'s own comment on why these might
    # not exist at all - no passwordless `sudo tc` on rpi#1 yet) - one named entry per (loss level,
    # mode) combination, all three levels in the *same* two JSON files/benchmark groups (matching
    # tool: customBiggerIsBetter/customSmallerIsBetter's own "one direction per file" constraint)
    # rather than one file per level, so they render as one graph with six lines instead of six
    # separate graphs.
    if [ "$LOSS_TESTING_AVAILABLE" = "1" ]; then
        local throughput_entries="" latency_entries="" percent_entries=""
        for pct in $LOSS_LEVELS_PCT; do
            for mode in besteffort reliable; do
                local log="$LOG_DIR/loss${pct}_${mode}_server.log"
                local mbps lat lp
                mbps=$(grep -oP 'avg_mbps=\K[\d,.]+' "$log" 2>/dev/null | tail -1 | tr -d ',' || true)
                lat=$(grep -oP 'avg_latency_ms=\K[\d.]+' "$log" 2>/dev/null | tail -1 || true)
                lp=$(grep -oP 'loss_pct=\K[\d.]+' "$log" 2>/dev/null | tail -1 || true)
                [ -n "$throughput_entries" ] && throughput_entries="$throughput_entries,"
                throughput_entries="$throughput_entries{\"name\": \"$mode @ ${pct}% loss\", \"unit\": \"Mbps\", \"value\": ${mbps:-0}}"
                [ -n "$latency_entries" ] && latency_entries="$latency_entries,"
                latency_entries="$latency_entries{\"name\": \"$mode @ ${pct}% loss\", \"unit\": \"ms\", \"value\": ${lat:-0}}"
                [ -n "$percent_entries" ] && percent_entries="$percent_entries,"
                percent_entries="$percent_entries{\"name\": \"$mode @ ${pct}% loss\", \"unit\": \"%\", \"value\": ${lp:-0}}"
            done
        done
        printf '[%s]\n' "$throughput_entries" > loss-throughput-benchmark.json
        printf '[%s]\n' "$latency_entries" > loss-latency-benchmark.json
        # The most direct evidence of QoS roadmap #5 actually working: BEST_EFFORT's own loss_pct
        # vs RELIABLE's, side by side at each injected loss level, tracked over time.
        printf '[%s]\n' "$percent_entries" > loss-percent-benchmark.json
    fi
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

# Loss-injection runs: BEST_EFFORT vs RELIABLE at each of LOSS_LEVELS_PCT, under real tc/netem
# packet loss instead of a clean link - this is where RELIABLE's own retransmission is actually
# expected to matter (on the clean-link "reliable" run above, it costs ~nothing to measure,
# since nothing is ever lost to retransmit). Paced at LOSS_TEST_INTERVAL_SEC (see its own comment
# on why this isn't the firehose "-i 0" the clean-link runs use) rather than run at max throughput,
# since the point here is measuring how well RELIABLE actually recovers under loss, not how fast
# it goes. perf_server.c's own one-way latency stat (its own NTP-clock-sync caveat) is what makes
# this the closest thing to a "reliable QoS latency" benchmark this rig has - RELIABLE's
# retransmit-then-deliver path should show up as a measurably higher avg/max latency than
# BEST_EFFORT's just-drop-it one as loss increases, which throughput/loss_pct alone wouldn't reveal.
probe_loss_testing
if [ "$LOSS_TESTING_AVAILABLE" = "1" ]; then
    for pct in $LOSS_LEVELS_PCT; do
        if ! set_loss "$pct"; then
            echo "Failed to apply ${pct}% tc loss on rpi#1 - skipping this loss level" >&2
            set_loss 0
            continue
        fi
        run_paired_test "loss${pct}_besteffort" "perf_server" "perf_client" "-i $LOSS_TEST_INTERVAL_SEC -d $PERF_DURATION_SEC" \
            "$((PERF_DURATION_SEC + 30))" "-W $LOSS_TEST_COOLDOWN_SEC"
        run_paired_test "loss${pct}_reliable" "perf_server" "perf_client" "-i $LOSS_TEST_INTERVAL_SEC -d $PERF_DURATION_SEC -R" \
            "$((PERF_DURATION_SEC + 30))" "-R -W $LOSS_TEST_COOLDOWN_SEC"
        set_loss 0
    done
fi

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
