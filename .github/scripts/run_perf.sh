#!/usr/bin/env bash
# Hardware-in-the-loop performance test, run from the self-hosted runner (tickle-hil).
#
# Rebuilt 2026-09-22 (the user's own explicit request) around rmw_tickle/COMPARISON.MD's own §2-3
# TickLE-core HIL methodology (examples/perf_hil/tickle/*, run via run_scenario.sh, TickLE's real
# max send rate) instead of this script's own former tool (examples/linux/perf/{perf_client,
# perf_server}, examples/linux/ping_pong/{ping,pong} - a different, lower-pacing tool). Motivation:
# a real, concrete confusion this mismatch caused, not just a cosmetic inconsistency - Milestone 65's
# own dashboard-sourced "loss_pct -> 0.0%" claim for the ACKNACK bitmap widening was measured under
# the *old* tool and did not hold up when re-measured with COMPARISON.MD's own real max-rate
# scenario 4 (1.9-8.1% loss). This script (and the dashboard it feeds) now measures the same thing
# COMPARISON.MD documents, so the two can never silently diverge like that again.
#
# Roles (fixed, per the two dedicated test Pis - unchanged from before this rewrite):
#   rpi#1 - client role
#   rpi#2 - server role
#
# Assumes:
#   - This runner already has an SSH key at ~/.ssh/tickle_ci_ed25519 authorized for
#     the "ci" account on both Pis (see .github/scripts/README.md for setup).
#   - Both Pis already have ~/tickle cloned (this script only fetches/checks out).
set -euo pipefail

RPI_CLIENT_HOST="${RPI_CLIENT_HOST:-10.1.1.214}" # rpi#1
RPI_SERVER_HOST="${RPI_SERVER_HOST:-10.1.1.213}" # rpi#2
SSH_USER="ci"
SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
REMOTE_DIR="tickle"
SCEN_ROOT="examples/perf_hil/tickle"

# Same separate-network reasoning as before this rewrite: RPI_CLIENT_HOST/RPI_SERVER_HOST above are
# the Pis' own *management* addresses (this script's own SSH target); the actual TickLE test traffic
# always broadcasts to PERF_LINK_BROADCAST instead (examples/linux/common/cli_opts.c's own -b
# default, which every perf_hil/tickle/*/client.c also uses unchanged) - a real, separate link
# probe_loss_testing() below needs rpi#1's own outgoing interface *for*, not whichever one happens
# to route toward RPI_SERVER_HOST's management IP.
PERF_LINK_BROADCAST="${PERF_LINK_BROADCAST:-192.168.10.255}"

SSH_OPTS=(-i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new)

ssh_run() {
    local host="$1"
    shift
    # "$@" is a command line meant to run on $host - it is supposed to expand remotely.
    # shellcheck disable=SC2029
    ssh "${SSH_OPTS[@]}" "$SSH_USER@$host" "$@"
}

LOG_DIR="$(mktemp -d)"

# QoS roadmap #5 (RELIABILITY/RELIABLE) loss-injection (scenario 4 only, now) - same tc/netem
# machinery as before this rewrite, just no longer feeding a whole separate matrix of loss${pct}_*
# scenarios (COMPARISON.MD §3 only measures reliable_throughput at 0/1/5% tc loss, not 10% - that
# was this script's own prior convention, not COMPARISON.MD's).
CLIENT_IFACE=""          # resolved below; set_loss() below is always a safe no-op while empty
LOSS_TESTING_AVAILABLE=0 # 1 once both the interface and passwordless sudo tc are confirmed

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

probe_loss_testing() {
    CLIENT_IFACE=$(ssh_run "$RPI_CLIENT_HOST" "ip route get $PERF_LINK_BROADCAST" 2>/dev/null |
        grep -oP 'dev \K\S+' | head -1 || true)
    if [ -z "$CLIENT_IFACE" ]; then
        echo "Could not resolve rpi#1's outgoing interface for $PERF_LINK_BROADCAST - skipping tc loss injection (scenario 4 will only run at 0%)" >&2
        return
    fi
    echo "rpi#1's test-link traffic (toward $PERF_LINK_BROADCAST) goes out $CLIENT_IFACE"

    if ssh_run "$RPI_CLIENT_HOST" "sudo -n tc qdisc replace dev $CLIENT_IFACE root netem loss 1%" >/dev/null 2>&1; then
        set_loss 0
        LOSS_TESTING_AVAILABLE=1
    else
        CLIENT_IFACE="" # also disarms set_loss()'s own exit-trap cleanup - nothing was ever applied
        echo "rpi#1 cannot run 'sudo tc' non-interactively - scenario 4 will only run at 0% tc loss" \
            "(see .github/scripts/README.md's own sudoers requirement)" >&2
    fi
}

# Fragment the "Performance Test" workflow hands to .github/scripts/publish_dashboard.sh for the
# Raspberry Pi row of https://tsnlab.github.io/tickle/dev/bench/. Seed it as a failure now and
# rewrite it once results exist, so an early abort (build failure, SSH timeout) still leaves the
# dashboard an honest red.
FRAG="${DASHBOARD_FRAGMENT:-perf-frag.json}"
printf '{"build":"fail","integration":"fail","commit":"%s","commit_short":"%s","date":"%s"}\n' \
    "$(git rev-parse HEAD)" "$(git rev-parse --short HEAD)" "$(date -u +%Y-%m-%dT%H:%MZ)" > "$FRAG"

# Builds every scenario's own client+server on both Pis, in parallel - mirrors the former
# update_and_build()'s own parallel-SSH/wait shape. build.sh <scenario> (examples/perf_hil/tickle/
# build.sh) does a *one-time* `make install PREFIX=~/tickle_local_install`, guarded by a plain
# `[ -f .../libtickle.a ]` check - left alone across runs, a later push with new TickLE source would
# silently keep building every scenario against a stale install. Force-cleaned here every run so
# that can never happen (the same staleness class of bug this whole codebase's own history has hit
# more than once - see e.g. rmw_tickle/PLAN.md's own pip-install-staleness lesson).
update_and_build() {
    local sha
    sha="$(git rev-parse HEAD)"
    echo "Checking out $sha and building all $((${#SCENARIOS[@]})) scenarios on both Pis..."

    local pids=()
    for host in "$RPI_CLIENT_HOST" "$RPI_SERVER_HOST"; do
        {
            local build_cmds="set -e
cd ~/$REMOTE_DIR
git fetch --quiet origin
git reset --hard --quiet $sha
git clean -fdq
rm -rf ~/tickle_local_install"
            for scenario in "${SCENARIOS[@]}"; do
                build_cmds="$build_cmds
cd ~/$REMOTE_DIR/$SCEN_ROOT && ./build.sh $scenario"
            done
            ssh_run "$host" "$build_cmds" > "$LOG_DIR/build_$host.log" 2>&1
        } &
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

# Every scenario's own client.c/server.c prints "RESULT: framework=tickle scenario=<name>
# role=<client|server> field=value field=value ..." (a uniform key=value shape, confirmed directly
# against all 9 scenarios' own source) - one small generic extractor instead of nine bespoke regex
# blocks. Echoes empty string (not an error) when the field/file is missing, matching every prior
# "|| true" convention in this script - a missing number should degrade the one JSON/dashboard field
# that needed it, never abort the whole run.
result_field() {
    local log="$1" field="$2"
    grep -oP "(?<= )${field}=\K[^\s]+" "$log" 2>/dev/null | tail -1 || true
}

# Runs one scenario invocation. run_scenario.sh (examples/perf_hil/tickle/run_scenario.sh) is
# itself the orchestrator - it SSHes to *both* Pis on its own (RPI_CLIENT/RPI_SERVER, hardcoded
# inside it) using whatever host it's invoked from as the origin, mirroring the cyclonedds/fastdds
# twins' own run_scenario.sh pattern. It must run directly on this runner (which is where
# ~/.ssh/tickle_ci_ed25519 actually lives, per this script's own top-of-file assumption) - an
# earlier version of this function wrapped it in its own extra `ssh_run "$RPI_CLIENT_HOST" ...`
# hop, which meant rpi#1 itself (not this runner) tried to SSH onward to rpi#2/itself with a key
# it doesn't have - every invocation failed in well under a second (real CI evidence: all-N/A
# results despite a "successful" exit, `|| true` below swallowing the real error) rather than
# taking the real several-second scenario runtime. Captures combined output (run_scenario.sh's
# own stdout already interleaves the client's and server's own RESULT: lines, per its own script)
# to $LOG_DIR/<label>.log for result_field() above to read back. `pre_client_sleep` is forwarded
# as run_scenario.sh's own PRE_CLIENT_SLEEP env override (0 for history_depth_burst_loss - see
# that scenario's own comment on why the usual pre-client sleep would hide the real race; the
# run_scenario.sh default, 3, for everything else - passed explicitly here anyway so this function
# never silently depends on that script's own default not changing later).
run_scenario() {
    local label="$1" scenario="$2" pre_client_sleep="$3" client_args="${4:-}"
    echo "== $label =="
    (
        cd "$SCEN_ROOT" &&
            PRE_CLIENT_SLEEP="$pre_client_sleep" ./run_scenario.sh "$scenario" $client_args
    ) > "$LOG_DIR/${label}.log" 2>&1 || true
}

# --- Scenario definitions, matching rmw_tickle/COMPARISON.MD §2-3 exactly ---
#
# best_effort_latency/reliable_latency/best_effort_throughput/deadline_miss_detection: no args,
# fixed default run - client.c/server.c take none (confirmed: no argv parsing in either).
#
# durability_late_join: two runs are actually needed, not one - confirmed directly against
# client.c/server.c's own source (not assumed): the DURABLE/VOLATILE setting lives on the
# server.c (Publisher) side's own -D flag, and each RESULT line only reports that one run's own
# received=<N> count (no combined durable-vs-volatile line exists anywhere in either file). Since
# run_scenario.sh forwards CLIENT_ARGS identically to both sides, passing "-D" reaches the server
# where it matters; client.c's own -D is bookkeeping-only (its own doc comment says so) and is
# harmless either way.
#
# history_depth_burst_loss: depth=8/interval=0.05s/count=160 are all fixed in the binary itself
# (not CLI-configurable) - only pause_s varies. "Within depth" needs a stall shorter than the
# depth*interval=0.4s eviction window; "beyond depth" is the binary's own default (pause_s=3.0,
# confirmed via source - well past it). No prior automated run of this scenario exists to recover
# an exact "within depth" value from (this scenario was always run by hand until now) - 0 is the
# simplest, most defensible choice (the Subscriber joins essentially immediately, deep inside the
# window), used here rather than an arbitrary intermediate guess.
#
# reliable_throughput: 0/1/5% tc loss (COMPARISON.MD's own three rows - not 10%, this script's own
# former convention, not COMPARISON.MD's own).
#
# liveliness_loss_detection: lease=1.0/2.0/4.0s, matching COMPARISON.MD's own already-published
# values (and Milestone 65's own follow-up interest in the ~3s node-level sweep ceiling).
#
# lifespan_expiry: pause_s=1.0/1.5/2.0s with interval=0.02s/lifespan=0.1s/count=250 - the exact
# slope-test methodology rmw_tickle/PLAN.md's own DDS semantic-parity backlog row 5 and
# COMPARISON.MD §6 item 8 already verified against the expected formula on real HIL.

SCENARIOS=(best_effort_latency reliable_latency best_effort_throughput reliable_throughput
    durability_late_join history_depth_burst_loss deadline_miss_detection liveliness_loss_detection
    lifespan_expiry)

run_all_scenarios() {
    run_scenario "best_effort_latency" "best_effort_latency" 3
    run_scenario "reliable_latency" "reliable_latency" 3
    run_scenario "best_effort_throughput" "best_effort_throughput" 3
    run_scenario "deadline_miss_detection" "deadline_miss_detection" 3
    run_scenario "durability_volatile" "durability_late_join" 3
    run_scenario "durability_durable" "durability_late_join" 3 "-D"

    run_scenario "history_within_depth" "history_depth_burst_loss" 0 "-p 0"
    run_scenario "history_beyond_depth" "history_depth_burst_loss" 0

    probe_loss_testing
    run_scenario "reliable_throughput_0pct" "reliable_throughput" 3
    if [ "$LOSS_TESTING_AVAILABLE" = "1" ]; then
        for pct in 1 5; do
            if set_loss "$pct"; then
                run_scenario "reliable_throughput_${pct}pct" "reliable_throughput" 3
            else
                echo "Failed to apply ${pct}% tc loss on rpi#1 - skipping reliable_throughput @ ${pct}%" >&2
            fi
            set_loss 0
        done
    fi

    run_scenario "liveliness_lease_1_0" "liveliness_loss_detection" 3 "-T 1.0"
    run_scenario "liveliness_lease_2_0" "liveliness_loss_detection" 3 "-T 2.0"
    run_scenario "liveliness_lease_4_0" "liveliness_loss_detection" 3 "-T 4.0"

    for pause in 1.0 1.5 2.0; do
        local label="lifespan_pause_${pause//./_}"
        run_scenario "$label" "lifespan_expiry" 3 "-i 0.02 -T 0.1 -n 250 -p $pause"
    done
}

# --- Job-summary rendering (mirrors COMPARISON.MD's own §3 table shape directly) ---

summarize() {
    {
        echo "## TickLE HIL scenario results (rmw_tickle/COMPARISON.MD §2-3 methodology)"
        echo
        echo "| Scenario | Condition | Result |"
        echo "|---|---|---|"

        local lat_avg lat_loss
        lat_avg=$(result_field "$LOG_DIR/best_effort_latency.log" "rtt_avg_ms")
        lat_loss=$(result_field "$LOG_DIR/best_effort_latency.log" "loss_pct")
        echo "| best_effort_latency | - | avg ${lat_avg:-N/A}ms, ${lat_loss:-N/A}% loss |"

        lat_avg=$(result_field "$LOG_DIR/reliable_latency.log" "rtt_avg_ms")
        lat_loss=$(result_field "$LOG_DIR/reliable_latency.log" "loss_pct")
        echo "| reliable_latency | - | avg ${lat_avg:-N/A}ms, ${lat_loss:-N/A}% loss |"

        local recv loss
        recv=$(result_field "$LOG_DIR/best_effort_throughput.log" "recv")
        loss=$(result_field "$LOG_DIR/best_effort_throughput.log" "loss_pct")
        echo "| best_effort_throughput | max rate, 8s | recv ${recv:-N/A}, ${loss:-N/A}% loss |"

        for pct in 0pct 1pct 5pct; do
            local log="$LOG_DIR/reliable_throughput_${pct}.log"
            recv=$(result_field "$log" "recv")
            loss=$(result_field "$log" "loss_pct")
            echo "| reliable_throughput | tc loss=${pct%pct}% | recv ${recv:-N/A}, ${loss:-N/A}% loss |"
        done

        local durable_recv volatile_recv
        durable_recv=$(result_field "$LOG_DIR/durability_durable.log" "received")
        volatile_recv=$(result_field "$LOG_DIR/durability_volatile.log" "received")
        echo "| durability_late_join | durable (-D) | recv ${durable_recv:-N/A} |"
        echo "| durability_late_join | volatile (default) | recv ${volatile_recv:-N/A} |"

        recv=$(result_field "$LOG_DIR/history_within_depth.log" "recv")
        echo "| history_depth_burst_loss | within depth | recv ${recv:-N/A}/160 |"
        recv=$(result_field "$LOG_DIR/history_beyond_depth.log" "recv")
        echo "| history_depth_burst_loss | beyond depth | recv ${recv:-N/A}/160 |"

        local writer_misses
        writer_misses=$(result_field "$LOG_DIR/deadline_miss_detection.log" "writer_misses")
        echo "| deadline_miss_detection | - | writer_misses ${writer_misses:-N/A} |"

        for lease in 1_0 2_0 4_0; do
            local detect
            detect=$(result_field "$LOG_DIR/liveliness_lease_${lease}.log" "detect_latency_ms")
            echo "| liveliness_loss_detection | lease=${lease//_/.}s | detect ${detect:-N/A}ms |"
        done

        for pause in 1_0 1_5 2_0; do
            local lost
            lost=$(result_field "$LOG_DIR/lifespan_pause_${pause}.log" "lost")
            echo "| lifespan_expiry | pause=${pause//_/.}s | lost ${lost:-N/A} |"
        done
    } | tee -a "${GITHUB_STEP_SUMMARY:-/dev/stdout}"
}

# --- github-action-benchmark JSON (one pair per scenario/direction that needs one - see this
# script's own top comment for the "why one file per direction" split) ---

write_benchmark_json() {
    local v

    v=$(result_field "$LOG_DIR/best_effort_latency.log" "rtt_avg_ms")
    printf '[{"name": "avg RTT", "unit": "ms", "value": %s}]\n' "${v:-0}" > best_effort_latency-benchmark.json

    v=$(result_field "$LOG_DIR/reliable_latency.log" "rtt_avg_ms")
    printf '[{"name": "avg RTT", "unit": "ms", "value": %s}]\n' "${v:-0}" > reliable_latency-benchmark.json

    v=$(result_field "$LOG_DIR/best_effort_throughput.log" "recv")
    printf '[{"name": "recv msgs", "unit": "count", "value": %s}]\n' "${v:-0}" > best_effort_throughput-benchmark.json

    local mbps_entries="" loss_entries=""
    for pct in 0pct 1pct 5pct; do
        local log="$LOG_DIR/reliable_throughput_${pct}.log"
        local recv loss
        recv=$(result_field "$log" "recv")
        loss=$(result_field "$log" "loss_pct")
        [ -n "$mbps_entries" ] && mbps_entries="$mbps_entries,"
        mbps_entries="$mbps_entries{\"name\": \"recv @ ${pct%pct}%\", \"unit\": \"count\", \"value\": ${recv:-0}}"
        if [ "$pct" != "0pct" ]; then
            [ -n "$loss_entries" ] && loss_entries="$loss_entries,"
            loss_entries="$loss_entries{\"name\": \"loss_pct @ ${pct%pct}%\", \"unit\": \"%\", \"value\": ${loss:-0}}"
        fi
    done
    printf '[%s]\n' "$mbps_entries" > reliable_throughput_recv-benchmark.json
    printf '[%s]\n' "$loss_entries" > reliable_throughput_loss-benchmark.json

    local durable_recv volatile_recv
    durable_recv=$(result_field "$LOG_DIR/durability_durable.log" "received")
    volatile_recv=$(result_field "$LOG_DIR/durability_volatile.log" "received")
    printf '[{"name": "durable recv", "unit": "count", "value": %s}]\n' "${durable_recv:-0}" > durability_late_join_durable-benchmark.json
    printf '[{"name": "volatile recv", "unit": "count", "value": %s}]\n' "${volatile_recv:-0}" > durability_late_join_volatile-benchmark.json

    local within beyond
    within=$(result_field "$LOG_DIR/history_within_depth.log" "recv")
    beyond=$(result_field "$LOG_DIR/history_beyond_depth.log" "recv")
    printf '[{"name": "within depth", "unit": "count", "value": %s}, {"name": "beyond depth", "unit": "count", "value": %s}]\n' \
        "${within:-0}" "${beyond:-0}" > history_depth_burst_loss-benchmark.json

    local writer_misses
    writer_misses=$(result_field "$LOG_DIR/deadline_miss_detection.log" "writer_misses")
    printf '[{"name": "writer_misses", "unit": "count", "value": %s}]\n' "${writer_misses:-0}" > deadline_miss_detection-benchmark.json

    local live_entries=""
    for lease in 1_0 2_0 4_0; do
        local detect
        detect=$(result_field "$LOG_DIR/liveliness_lease_${lease}.log" "detect_latency_ms")
        [ -n "$live_entries" ] && live_entries="$live_entries,"
        live_entries="$live_entries{\"name\": \"lease=${lease//_/.}s\", \"unit\": \"ms\", \"value\": ${detect:-0}}"
    done
    printf '[%s]\n' "$live_entries" > liveliness_loss_detection-benchmark.json

    local life_entries=""
    for pause in 1_0 1_5 2_0; do
        local lost
        lost=$(result_field "$LOG_DIR/lifespan_pause_${pause}.log" "lost")
        [ -n "$life_entries" ] && life_entries="$life_entries,"
        life_entries="$life_entries{\"name\": \"pause=${pause//_/.}s\", \"unit\": \"count\", \"value\": ${lost:-0}}"
    done
    printf '[%s]\n' "$life_entries" > lifespan_expiry-benchmark.json
}

# --- Platform-status dashboard fragment (dashboard.py's own "perf" section, 11-field set matching
# the new scenario methodology - see .github/scripts/dashboard.py's own _row()/render_block()) ---

write_dashboard_fragment() {
    local be_lat rel_lat be_tput rel_tput0 rel_loss1 rel_loss5 live2 life1_5 hist_beyond dur_durable dur_volatile

    be_lat=$(result_field "$LOG_DIR/best_effort_latency.log" "rtt_avg_ms")
    rel_lat=$(result_field "$LOG_DIR/reliable_latency.log" "rtt_avg_ms")
    be_tput=$(result_field "$LOG_DIR/best_effort_throughput.log" "recv")
    rel_tput0=$(result_field "$LOG_DIR/reliable_throughput_0pct.log" "recv")
    rel_loss1=$(result_field "$LOG_DIR/reliable_throughput_1pct.log" "loss_pct")
    rel_loss5=$(result_field "$LOG_DIR/reliable_throughput_5pct.log" "loss_pct")
    live2=$(result_field "$LOG_DIR/liveliness_lease_2_0.log" "detect_latency_ms")
    life1_5=$(result_field "$LOG_DIR/lifespan_pause_1_5.log" "lost")
    hist_beyond=$(result_field "$LOG_DIR/history_beyond_depth.log" "recv")
    dur_durable=$(result_field "$LOG_DIR/durability_durable.log" "received")
    dur_volatile=$(result_field "$LOG_DIR/durability_volatile.log" "received")

    # A round trip happened at all (latency got a real avg) => integration pass.
    local integ=fail
    if [ -n "${be_lat:-}" ]; then
        integ=pass
    fi

    cat > "$FRAG" <<EOF
{
  "build": "pass",
  "integration": "$integ",
  "commit": "$(git rev-parse HEAD)",
  "commit_short": "$(git rev-parse --short HEAD)",
  "date": "$(date -u +%Y-%m-%dT%H:%MZ)",
  "run_url": "${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-tsnlab/tickle}/actions/runs/${GITHUB_RUN_ID:-0}",
  "be_latency_ms": ${be_lat:-null},
  "rel_latency_ms": ${rel_lat:-null},
  "be_throughput_recv": ${be_tput:-null},
  "rel_throughput_recv_0pct": ${rel_tput0:-null},
  "rel_loss_1pct": ${rel_loss1:-null},
  "rel_loss_5pct": ${rel_loss5:-null},
  "liveliness_detect_2s_ms": ${live2:-null},
  "lifespan_lost_1_5s": ${life1_5:-null},
  "history_beyond_depth_recv": ${hist_beyond:-null},
  "durability_durable_recv": ${dur_durable:-null},
  "durability_volatile_recv": ${dur_volatile:-null}
}
EOF
}

update_and_build
run_all_scenarios
summarize
write_benchmark_json
write_dashboard_fragment
