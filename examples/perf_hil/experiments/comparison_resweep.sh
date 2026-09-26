#!/usr/bin/env bash
# Re-measure COMPARISON.md's native (no rmw) sections at the current main, all three frameworks
# in ONE session, so every column is comparable to the others.
#
# Why (2026-09-24): since §3/§3a were last measured, TickLE's core has changed underneath them -
# strict RELIABLE ordering, the O(1) reorder buffer, the watermark drains (e300fe72), the
# piggybacked heartbeat (7e034ef2, native harness only if armed), the oversize-flush fix
# (477293da), multi-part discovery (82089b2f) and the create-default fix (718eaacf). Only §3b was
# re-measured after the ordering work. The rest of the document describes code that no longer
# exists. §5 (rmw) is NOT covered: rmw performance on the rig waits for the rig upgrade (the rpis
# have no ROS 2; user decision 2026-09-24).
#
# Parts, each framework interleaved within every rep so drift lands on all three alike:
#   A  §3 scenarios 1-3, 5-7, 9. Latency (1, 2) and best_effort_throughput (3) get EXPLICIT,
#      identical args for all three, because the harness defaults differ: the DDS run_scenario.sh
#      defaults to "-i 0.1 -d 10" and TickLE's to "-d 10", and every latency client's own default
#      interval is 1.0 s, so a bare invocation pings TickLE at 1 Hz and the DDS pair at 10 Hz.
#      Found while writing this script. Whether any published §3 cell was taken that way is to be
#      checked against the document, not assumed. Durability runs both VOLATILE and -D; history
#      runs within (-p 0) and beyond depth; lifespan runs at run_perf.sh's three pauses; deadline
#      keeps each harness's defaults, because its flags genuinely differ by framework.
#   B  §3a - reliable_throughput -d 8 at tc loss 0/1/5/20/50%: TickLE -K 64 and -K 1024, DDS at
#      default max_blocking_time and at -B 1 (ms).
#   C  §3 scenario 8, liveliness - TickLE only, via run_perf.sh's own bespoke kill -9 orchestration.
#      The DDS twins need their own orchestration, which does not exist in a reusable form; their
#      §3 liveliness cells stay dated and are marked so.
#   D  §3b - keepall_three_way.sh, unchanged.
#
# HOW TO READ IT, written before running:
#   - A cell whose leftover guard fired (CONTAMINATED) is not used.
#   - A run with no RESULT line is recorded as such and never as a zero.
#   - Numbers are compared across frameworks WITHIN this file only. Against the currently published
#     figures they are compared only as "moved / did not move", because the rig carries day-to-day
#     offsets (§5's +13us lesson).
#   - For TickLE, any cell that moves by more than the spread of its own 3 reps against the
#     published value is reported to Dev as a possible regression before the document changes.
#   - The DDS columns are the control: DDS code did not change, so if the DDS columns move as much
#     as TickLE's, the movement is the rig, not TickLE.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PH/../.." && pwd)"

if [ "${RIG_LOCK_HELD_HIL:-${RIG_LOCK_HELD:-0}}" != "1" ]; then
    exec "$PH/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT=10.1.1.214
RPI_SERVER=10.1.1.213
REPS="${REPS:-3}"
OUT="${OUT:-/tmp/tickle_comparison_resweep_$(date +%Y%m%d-%H%M%S).txt}"
ln -sfn "$OUT" /tmp/tickle_comparison_resweep_latest.txt

ssh_h() { local h=$1; shift; ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$h" "$@"; }
sl() { if [ "$1" = 0 ]; then ssh_h "$RPI_CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true
       else ssh_h "$RPI_CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss $1%"; fi; }
trap 'sl 0' EXIT

: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }

SHA="$(git -C "$REPO" rev-parse origin/main)"
say "=== COMPARISON.md native re-sweep, $(date -Is), ${REPS} reps, main $SHA ==="

say "--- deploying $SHA and building every scenario for all three frameworks on both rpis ---"
SCENS="best_effort_latency reliable_latency best_effort_throughput reliable_throughput durability_late_join history_depth_burst_loss deadline_miss_detection liveliness_loss_detection lifespan_expiry"
# Bare `wait` returns 0 however the background jobs ended, so a BUILD FAILED here used to print and
# the resweep would carry on measuring whatever binary was left from the previous checkout. Each pid
# is waited on individually instead (campaign_sweep.sh, d32e7406).
build_pids=()
for host in "$RPI_CLIENT" "$RPI_SERVER"; do
    ssh_h "$host" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cd examples/perf_hil
for fw in tickle cyclonedds fastdds; do for s in $SCENS; do
  (cd \$fw && ./build.sh \$s >/tmp/resweep_build_\${fw}_\$s.log 2>&1) || { echo \"BUILD FAILED: \$fw \$s\"; tail -5 /tmp/resweep_build_\${fw}_\$s.log; exit 1; }
done; done
echo \"built on \$(hostname) at \$(git -C ~/tickle rev-parse --short HEAD)\"" &
    build_pids+=($!)
done
build_failed=0
for pid in "${build_pids[@]}"; do wait "$pid" || build_failed=1; done
[ "$build_failed" = 0 ] || { say "BUILD FAILED on at least one rpi - see above. Not measuring."; exit 1; }
say "$(ssh_h "$RPI_CLIENT" 'git -C ~/tickle rev-parse HEAD') client / $(ssh_h "$RPI_SERVER" 'git -C ~/tickle rev-parse HEAD') server"

# Leftover guard, as keepall_three_way.sh: identify by /proc/PID/exe, wait, never kill, tag.
rig_leftovers() {
    local host out=""
    for host in "$RPI_CLIENT" "$RPI_SERVER"; do
        # shellcheck disable=SC2016 # expanded on the rpi, not here
        out+=$(ssh_h "$host" '
            for d in /proc/[0-9]*; do
                e=$(readlink "$d/exe" 2>/dev/null) || continue
                case "$e" in */tickle/examples/perf_hil/*/client|*/tickle/examples/perf_hil/*/server)
                    fw=${e%/*/*}; fw=${fw##*/}; printf "%s:%s:%s " "$(hostname)" "$fw" "${d#/proc/}";;
                esac
            done' 2>/dev/null)
    done
    printf '%s' "$out"
}
wait_rig_quiet() {
    local left _
    for _ in $(seq 1 30); do
        left=$(rig_leftovers)
        [ -z "$left" ] && { echo none; return 0; }
        sleep 1
    done
    echo "$left"
}

# One cell: $1 part label, $2 framework, $3 scenario, $4 PRE_CLIENT_SLEEP (TickLE only), rest args.
cell() {
    local label=$1 fw=$2 scen=$3 pre=$4; shift 4
    local left; left=$(wait_rig_quiet)
    local res
    # shellcheck disable=SC2068 # args deliberately word-split into the harness's argv
    res=$(cd "$PH/$fw" && PRE_CLIENT_SLEEP="$pre" timeout 180 ./run_scenario.sh "$scen" $@ 2>/dev/null | grep '^RESULT:' | tr '\n' ' ' || true)
    [ -n "$res" ] || res="NO RESULT LINE"
    say "$label $fw | leftover_before=$left$([ "$left" != none ] && echo ' CONTAMINATED') | $res"
}

say ""; say "=== A: section 3 scenarios ==="
specA=(
  "best_effort_latency|best_effort_latency|3|-i 0.1 -d 10"
  "reliable_latency|reliable_latency|3|-i 0.1 -d 10"
  "best_effort_throughput|best_effort_throughput|3|-d 8"
  "deadline_miss_detection|deadline_miss_detection|3|"
  "durability_volatile|durability_late_join|3|"
  "durability_durable|durability_late_join|3|-D"
  "history_within_depth|history_depth_burst_loss|0|-p 0"
  "history_beyond_depth|history_depth_burst_loss|0|"
  # lifespan_expiry at 0, as history_depth_burst_loss: server.c stalls for -p before creating
  # its Subscriber, so a 3s client start would put the Subscriber there before the first sample.
  "lifespan_pause_1_0|lifespan_expiry|0|-i 0.02 -T 0.1 -n 250 -p 1.0"
  "lifespan_pause_1_5|lifespan_expiry|0|-i 0.02 -T 0.1 -n 250 -p 1.5"
  "lifespan_pause_2_0|lifespan_expiry|0|-i 0.02 -T 0.1 -n 250 -p 2.0"
)
for spec in "${specA[@]}"; do
    IFS='|' read -r label scen pre args <<<"$spec"
    for rep in $(seq 1 "$REPS"); do
        for fw in tickle cyclonedds fastdds; do
            # An empty $args means "the harness's own defaults", which differ per framework by
            # design (the DDS twins pace some scenarios with -i); passing nothing preserves that.
            # shellcheck disable=SC2086
            cell "A $label rep$rep" "$fw" "$scen" "$pre" $args
        done
    done
done

say ""; say "=== B: section 3a, reliable_throughput under injected loss ==="
for pct in 0 1 5 20 50; do
    sl "$pct"
    say "--- tc loss ${pct}% ---"
    for rep in $(seq 1 "$REPS"); do
        cell "B loss$pct K64 rep$rep" tickle reliable_throughput 3 -d 8 -K 64
        cell "B loss$pct K1024 rep$rep" tickle reliable_throughput 3 -d 8 -K 1024
        cell "B loss$pct Bdefault rep$rep" cyclonedds reliable_throughput 3 -d 8
        cell "B loss$pct B1ms rep$rep" cyclonedds reliable_throughput 3 -d 8 -B 1
        cell "B loss$pct Bdefault rep$rep" fastdds reliable_throughput 3 -d 8
        cell "B loss$pct B1ms rep$rep" fastdds reliable_throughput 3 -d 8 -B 1
    done
done
sl 0

say ""; say "=== C: section 3 scenario 8, liveliness (TickLE only) ==="
LD=tickle/examples/perf_hil/tickle/liveliness_loss_detection
for lease in 1.0 2.0 4.0; do
    for rep in $(seq 1 "$REPS"); do
        left=$(wait_rig_quiet)
        ssh_h "$RPI_SERVER" "cd ~/$LD; rm -f /tmp/resweep_live_server.log; nohup taskset -c 1-3 ./server -T $lease > /tmp/resweep_live_server.log 2>&1 < /dev/null &"
        ssh_h "$RPI_CLIENT" "cd ~/$LD; nohup taskset -c 1-3 ./client -T $lease > /tmp/resweep_live_client.log 2>&1 < /dev/null &"
        sleep 6
        # The client is identified by the name of the binary this script just started, on a host
        # where the leftover guard above found no perf_hil client alive (CLAUDE.md rule 3).
        ssh_h "$RPI_CLIENT" "pkill -9 -x client" || true
        for _ in $(seq 1 10); do
            ssh_h "$RPI_SERVER" "grep -q '^RESULT:' /tmp/resweep_live_server.log" 2>/dev/null && break
            sleep 1
        done
        ssh_h "$RPI_SERVER" "pkill -INT -x server" 2>/dev/null || true
        sleep 1
        res=$(ssh_h "$RPI_SERVER" "grep '^RESULT:' /tmp/resweep_live_server.log" | tr '\n' ' ' || true)
        say "C liveliness_lease_$lease rep$rep tickle | leftover_before=$left$([ "$left" != none ] && echo ' CONTAMINATED') | ${res:-NO RESULT LINE}"
    done
done

say ""; say "=== D: section 3b, keepall_three_way.sh ==="
OUT3B="${OUT%.txt}_3b.txt"
OUT="$OUT3B" "$HERE/keepall_three_way.sh" >/dev/null 2>&1 || say "keepall_three_way.sh exited non-zero"
say "section 3b results: $OUT3B"
cat "$OUT3B" >> "$OUT" 2>/dev/null || true

sl 0
say ""; say "=== done $(date -Is); tc restored ==="
