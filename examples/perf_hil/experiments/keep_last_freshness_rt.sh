#!/usr/bin/env bash
# KEEP_LAST freshness by ROUND TRIP on the rig (the user's decision D3, 2026-10-18, and rule of 2026-10-19: across
# hosts only round trips are timed - the Pis' clocks drift and Ethernet PTP's error is large, so the one-way cells of
# keep_last_freshness.sh void across hosts). RESULTS rows 43/44 re-scored on freshness, all three frameworks.
#
# WHAT IS MEASURED (tickle/common/BenchFreshRt.h, the same code in all three frameworks' benches):
#   round-trip age = t_echo_rx - t_create, both on the SENDER's CLOCK_MONOTONIC (rpi#1): t_create the burst's due time,
#   t_echo_rx the arrival of the ECHO the reader (rpi#2) sends over a plain side UDP socket, not the middleware, the
#   moment its application takes the sample. The side channel's own delay is measured in the same run by a PING/PONG
#   on the same socket pair every 20 ms (the control). newest share = bursts whose newest sample was echoed / bursts
#   the sender scheduled; a superseded sample is not lost.
#   The side channel runs over the test link (eth0, 192.168.10.0/24), next to the middleware's own traffic.
#
# CELLS (history_depth_burst_loss, RELIABLE KEEP_LAST 8 on every writer and reader, P1, -i 0.05: 20 slots/s; every cell
# cross-host, rpi#1 the sender, rpi#2 the reader):
#   h43   -p 0, 160 samples           RESULTS row 43 (within depth 8), as published
#   h44   -p 3 (the reader stalls 3 s), 160 samples    RESULTS row 44 (beyond depth 8), as published
#   b8    -p 0, --burst 8, 800 samples (100 bursts of exactly the depth)
#   b16   -p 0, --burst 16, 1600 samples (100 bursts of twice the depth; the newest of each is still owed)
#   dly   h43 with `tc netem delay 1ms` on rpi#1's eth0 (the only Pi with tc): THE FAULT ARM. The data and the PINGs
#         leave through it, so the round-trip age and the side RTT must both rise by 1 ms; the qdisc actually in place
#         is recorded beside every run (QDISC lines) and the reader voids a run whose qdisc is not its cell's.
# Each rep runs every cell, dly right after h43, the frameworks interleaved in an order rotated per rep.
#
# HOW TO READ IT, written before the run and enforced in code by freshness_rt_summary.py (--prereg prints the rules;
# its --selftest exercises every one): sample counts first, echo accounting (lost echoes beyond 0.1% void a run), the
# side-channel control (a gap between frameworks counts only beyond the control's spread in the cell), the fault arm
# (every framework's age and side RTT must move by 1 ms +- 0.25 ms, or the session is NOT READ), then WIN / LOSE / TIE
# per cell on age p50, age p99 (read only where a run has >= 300 samples: b8, b16) and newest share.
#
# Usage:  SHA=<pushed sha> [REPS=3] [CELLS="h43 dly h44 b8 b16"] [FWS=...] [OUT=...] keep_last_freshness_rt.sh
#         Detached, with the lock taken inside and held once for every cell: rig_queue_keep_last_freshness_rt.sh.
#         ~20 s per run, 45 runs at REPS=3 plus the build: ~25 min.
set -uo pipefail
# shellcheck source=examples/perf_hil/experiments/pi_checkout.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$(dirname "${BASH_SOURCE[0]}")/pi_checkout.sh"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"

if [ "${RIG_LOCK_HELD_HIL:-${RIG_LOCK_HELD:-0}}" != "1" ]; then
    export RIG_LOCK_SCOPE=hil
    exec "$PH/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi

SHA=${SHA:?set SHA to a commit the Pis can fetch}
SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT=10.1.1.214
RPI_SERVER=10.1.1.213
REPS="${REPS:-3}"
CELLS="${CELLS:-h43 dly h44 b8 b16}"
FWS="${FWS:-tickle cyclonedds fastdds}"
RT_PORT="${RT_PORT:-47220}"
OUT="${OUT:-$HOME/rig_results_safe/keep_last_freshness_rt_${SHA:0:8}_$(date +%Y%m%d-%H%M%S).txt}"
mkdir -p "$(dirname "$OUT")"
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

ssh_h() {
    local h=$1
    shift
    ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$h" "$@"
}

qdisc_now() { ssh_h "$RPI_CLIENT" "tc qdisc show dev eth0" </dev/null 2>&1 | head -1; }
shaped_by_us=0
set_delay() { # on|off on rpi#1's eth0
    if [ "$1" = on ]; then
        shaped_by_us=1
        ssh_h "$RPI_CLIENT" "sudo -n tc qdisc replace dev eth0 root netem delay 1ms" </dev/null
    else
        ssh_h "$RPI_CLIENT" "sudo -n tc qdisc del dev eth0 root 2>/dev/null; true" </dev/null
        case "$(qdisc_now)" in *netem*) return 1 ;; esac
        shaped_by_us=0
    fi
}
# The one EXIT trap: a run killed inside dly must not leave rpi#1 shaped; then how the run ended.
finish() {
    if [ "$shaped_by_us" = 1 ]; then
        set_delay off || echo "FATAL: netem still on $RPI_CLIENT eth0 after del - the rig is left shaped" >>"$OUT"
    fi
    echo "=== ended $(date -Is) qdisc on $RPI_CLIENT: $(qdisc_now) ===" >>"$OUT"
}
trap finish EXIT

# label|args. -i 0.05 explicitly on every cell: the DDS twins' run_scenario.sh would otherwise put their own default
# -i 0.1 in front of an empty argument list (comparison_resweep.sh's header).
cell_spec() {
    case "$1" in
    h43 | dly) echo "-i 0.05 -n 160 -p 0 --fresh" ;;
    h44) echo "-i 0.05 -n 160 --fresh" ;;
    b8) echo "-i 0.05 -n 800 -p 0 --fresh --burst 8" ;;
    b16) echo "-i 0.05 -n 1600 -p 0 --fresh --burst 16" ;;
    *) return 1 ;;
    esac
}
for c in $CELLS; do cell_spec "$c" >/dev/null || { echo "unknown cell '$c'" >&2; exit 2; }; done

say "=== KEEP_LAST freshness by round trip, $(date -Is), sha $SHA, reps $REPS, cells: $CELLS, frameworks: $FWS ==="
for c in $CELLS; do say "    cell $c: $(cell_spec "$c")"; done

# The rig must be unshaped before we start: a netem left by a killed job would be read as freshness.
for h in "$RPI_CLIENT" "$RPI_SERVER"; do
    q=$(ssh_h "$h" "tc qdisc show dev eth0" </dev/null 2>&1)
    case "$q" in *netem* | *tbf*)
        say "REFUSED: $h eth0 is shaped ($q)"
        exit 1
        ;;
    esac
done
ssh_h "$RPI_CLIENT" "sudo -n tc qdisc show dev eth0 >/dev/null" </dev/null ||
    { say "REFUSED: sudo -n tc does not work on $RPI_CLIENT (the fault arm needs it)"; exit 1; }

say "--- deploying $SHA and building history_depth_burst_loss for every framework on both Pis ---"
build_pids=()
for host in "$RPI_CLIENT" "$RPI_SERVER"; do
    ssh_h "$host" "set -e
cd ~/tickle && $(pi_checkout "$SHA") && git clean -fdqx -e install -e build -e log
cd examples/perf_hil
for fw in $FWS; do
  (cd \$fw && ./build.sh history_depth_burst_loss >/tmp/klfrt_build_\$fw.log 2>&1) || { echo \"BUILD FAILED: \$fw\"; tail -5 /tmp/klfrt_build_\$fw.log; exit 1; }
done
[ -f tickle/common/BenchFreshRt.h ] || { echo 'no BenchFreshRt.h at this sha'; exit 1; }
echo \"built on \$(hostname) at \$(git -C ~/tickle rev-parse --short HEAD)\"" </dev/null &
    build_pids+=($!)
done
build_failed=0
for pid in "${build_pids[@]}"; do wait "$pid" || build_failed=1; done
[ "$build_failed" = 0 ] || { say "BUILD FAILED on at least one Pi - see above. Not measuring."; exit 1; }

# The echo target: rpi#1's own test-link address, which the reader on rpi#2 sends its ECHOes and PONGs to.
CLIENT_IP=$(ssh_h "$RPI_CLIENT" "ip -4 -o addr show dev eth0" </dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
[ -n "$CLIENT_IP" ] || { say "REFUSED: no IPv4 address on $RPI_CLIENT's eth0 for the side channel"; exit 1; }
RT="--fresh-rt-listen $RT_PORT --fresh-rt $CLIENT_IP:$RT_PORT"
say "side channel: rpi#2 -> $CLIENT_IP:$RT_PORT over eth0 (echo per take, ping every 20 ms)"

# Leftover guard (comparison_resweep.sh): a bench process by /proc/PID/exe, waited for, never killed; the run is
# tagged when one was still there.
rig_leftovers() {
    local host out=""
    for host in "$RPI_CLIENT" "$RPI_SERVER"; do
        # shellcheck disable=SC2016 # expanded on the Pi, not here
        out+=$(ssh_h "$host" '
            for d in /proc/[0-9]*; do
                e=$(readlink "$d/exe" 2>/dev/null) || continue
                case "$e" in */tickle/examples/perf_hil/*/client|*/tickle/examples/perf_hil/*/server)
                    fw=${e%/*/*}; fw=${fw##*/}; printf "%s:%s:%s " "$(hostname)" "$fw" "${d#/proc/}";;
                esac
            done' </dev/null 2>/dev/null)
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

run=0
for rep in $(seq 1 "$REPS"); do
    read -r -a fw_arr <<<"$FWS"
    k=$(((rep - 1) % ${#fw_arr[@]}))
    order=("${fw_arr[@]:$k}" "${fw_arr[@]:0:$k}")
    for c in $CELLS; do
        args="$(cell_spec "$c") $RT"
        for fw in "${order[@]}"; do
            run=$((run + 1))
            if [ "$c" = dly ]; then
                set_delay on || say "FRESH run=$run netem could not be set"
            else
                set_delay off || { say "FATAL: netem still on $RPI_CLIENT eth0 - stopping"; exit 1; }
            fi
            left=$(wait_rig_quiet)
            say "FRESH run=$run cell=$c fw=$fw rep=$rep topo=cross leftover_before=$left args=$args"
            say "QDISC run=$run $(qdisc_now)"
            # PRE_CLIENT_SLEEP 0, as comparison_resweep.sh's history cells: the TickLE server stalls before it even
            # creates its subscriber, so a later client would hand it the first samples late.
            # shellcheck disable=SC2086 # args deliberately word-split into the harness argv
            res=$(cd "$PH/$fw" && RPI_CLIENT=$RPI_CLIENT RPI_SERVER=$RPI_SERVER PRE_CLIENT_SLEEP=0 timeout 180 \
                ./run_scenario.sh history_depth_burst_loss $args 2>/dev/null | grep '^RESULT:' || true)
            if [ -z "$res" ]; then
                say "FRESH run=$run NO RESULT LINE"
            else
                while IFS= read -r line; do say "FRESH run=$run $line"; done <<<"$res"
            fi
            say "QDISC run=$run after $(qdisc_now)"
        done
        [ "$c" = dly ] && { set_delay off || { say "FATAL: netem still on $RPI_CLIENT eth0 - stopping"; exit 1; }; }
    done
done
say "=== done $(date -Is): $run runs ==="
say "read with: examples/perf_hil/experiments/freshness_rt_summary.py $OUT"
