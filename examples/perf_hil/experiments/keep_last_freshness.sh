#!/usr/bin/env bash
# KEEP_LAST freshness (the user's decision D3, 2026-10-18; QoS redesign step 8): RESULTS rows 43/44 re-scored on
# freshness, all three frameworks, one session, with a clock control and a fault arm the control must catch.
#
# WHAT IS MEASURED (tickle/common/BenchFreshness.h holds the definitions, in code, for all three frameworks):
#   age at take  = the moment the application takes a sample - the sample's creation time, where creation is the DUE
#                  time of its burst on the sender's schedule. CLOCK_REALTIME on both sides; across hosts the summary
#                  subtracts the offset clock_offset_probe.py measures (receiver - sender) before and after every run.
#   newest share = bursts whose newest sample the application took / bursts the sender scheduled. A burst is --burst B
#                  samples written back to back in one slot of the sender's schedule (seqs (k-1)B+1 .. kB; seq kB is
#                  its newest). With B = 1 every sample is its own burst. A sample superseded under KEEP_LAST is not
#                  lost: only the newest of each burst is owed.
#
# CELLS (history_depth_burst_loss, RELIABLE KEEP_LAST 8 on every writer and reader, P1, 20 slots/s; -i 0.05):
#   h43        cross-host, -p 0, B 1, 160 samples         RESULTS row 43 (within depth 8), as published, re-scored
#   h44        cross-host, -p 3 (the reader stalls 3 s), B 1, 160 samples      RESULTS row 44 (beyond depth 8), ditto
#   b8         cross-host, -p 0, B 8, 800 samples (100 bursts of exactly the depth)
#   b16        cross-host, -p 0, B 16, 1600 samples (100 bursts of twice the depth: half of every burst may be
#              superseded, and its newest is still owed)
#   s44, s16   h44 and b16 on one host (rpi#1 both ends): the same reading on one clock, where no offset can void it -
#              the fallback the plan names (QOS_PLAN D3, "measure on one host, or measure the offset on the rig") for
#              a cross-host cell the offset rule voids.
# The cross-host cells pass --fresh-dump: the server prints every age with its take time, and the summary corrects each
# by the offset interpolated at that time. The Pis' clocks drift ~30 us/s against each other (clock_offset_probe.py's
# header), so one offset for a ~20 s run would carry ~300 us of drift into U and void every transit-sized age; what
# U keeps is half the probe's best round trip (~100 us on the rig, a python probe) and 10% of the drift.
#   ctl        same host (rpi#1 both ends, as S1-S3: one clock), -p 0, B 1: THE CLOCK CONTROL. Every framework must read ages of
#              a transit (min within [-20 us, 5 ms], p50 under 5 ms), or the session is NOT READ.
#   fault      as ctl, with --fresh-skew-us 5000 on the sender: a deliberately wrong clock. The control MUST fail
#              here; if it passes, the control cannot fail and the session is NOT READ. 1 rep.
# Each rep runs every cell, the frameworks interleaved in an order rotated per rep, so drift lands on all three alike.
#
# HOW TO READ IT, written before the run and enforced in code by freshness_summary.py (its --selftest exercises every
# rule): sample counts first; a run without both RESULT lines, with fresh_no_stamp > 0 or a burst-size mismatch is VOID;
# a cross-host run without two good probes, whose offset uncertainty |after - before| / 2 + max(delay_min) / 2 exceeds
# its corrected age p50, or whose corrected minimum age is below -(uncertainty + 20 us), is VOID; then the control and
# the fault arm; then per cell and framework age p50, age p99 and newest share, scored WIN / DRAW / LOSE by
# docs/TESTING.md section 4 (TickLE's every rep beyond every rep of every scored vendor). Goodput (recv / sent) is
# printed as a diagnostic and never scored here (D3: freshness first in a KEEP_LAST cell).
# FALSIFIED (the instrument, not a framework): the control fails, or the fault arm passes it.
#
# Usage:  SHA=<pushed sha> [REPS=3] [CELLS="h43 h44 b8 b16 s44 s16 ctl fault"] [FWS=...] [OUT=...] keep_last_freshness.sh
#         Detached, with the lock taken inside: see rig_queue_keep_last_freshness.sh. ~25 s per run, 66 runs at REPS=3
#         plus the build: ~35 min.
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
CELLS="${CELLS:-h43 h44 b8 b16 s44 s16 ctl fault}"
FWS="${FWS:-tickle cyclonedds fastdds}"
PROBE_PORT="${PROBE_PORT:-47219}"
OUT="${OUT:-$HOME/rig_results_safe/keep_last_freshness_${SHA:0:8}_$(date +%Y%m%d-%H%M%S).txt}"
mkdir -p "$(dirname "$OUT")"
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

ssh_h() {
    local h=$1
    shift
    ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$h" "$@"
}

# One EXIT trap: the probe servers end on their own (--idle 3); this records how the run ended.
finish() { echo "=== ended $(date -Is) ===" >>"$OUT"; }
trap finish EXIT

# label|topo|args. -i 0.05 explicitly on every cell: the DDS twins' run_scenario.sh would otherwise put their own
# default -i 0.1 in front of an empty argument list (comparison_resweep.sh's header).
cell_spec() {
    case "$1" in
    h43) echo "cross|-i 0.05 -n 160 -p 0 --fresh-dump" ;;
    h44) echo "cross|-i 0.05 -n 160 --fresh-dump" ;;
    b8) echo "cross|-i 0.05 -n 800 -p 0 --fresh-dump --burst 8" ;;
    b16) echo "cross|-i 0.05 -n 1600 -p 0 --fresh-dump --burst 16" ;;
    s44) echo "same|-i 0.05 -n 160 --fresh" ;;
    s16) echo "same|-i 0.05 -n 1600 -p 0 --fresh --burst 16" ;;
    ctl) echo "same|-i 0.05 -n 160 -p 0 --fresh" ;;
    fault) echo "same|-i 0.05 -n 160 -p 0 --fresh --fresh-skew-us 5000" ;;
    *) return 1 ;;
    esac
}
for c in $CELLS; do cell_spec "$c" >/dev/null || { echo "unknown cell '$c'" >&2; exit 2; }; done

say "=== KEEP_LAST freshness, $(date -Is), sha $SHA, reps $REPS, cells: $CELLS, frameworks: $FWS ==="
for c in $CELLS; do say "    cell $c: $(cell_spec "$c")"; done

# The rig must be unshaped: these cells are lossless by design, and a netem left by a killed job would be read as
# freshness.
for h in "$RPI_CLIENT" "$RPI_SERVER"; do
    q=$(ssh_h "$h" "tc qdisc show dev eth0" </dev/null 2>&1)
    case "$q" in *netem* | *tbf*)
        say "REFUSED: $h eth0 is shaped ($q)"
        exit 1
        ;;
    esac
done

say "--- deploying $SHA and building history_depth_burst_loss for every framework on both Pis ---"
build_pids=()
for host in "$RPI_CLIENT" "$RPI_SERVER"; do
    ssh_h "$host" "set -e
cd ~/tickle && $(pi_checkout "$SHA") && git clean -fdqx -e install -e build -e log
cd examples/perf_hil
for fw in $FWS; do
  (cd \$fw && ./build.sh history_depth_burst_loss >/tmp/klf_build_\$fw.log 2>&1) || { echo \"BUILD FAILED: \$fw\"; tail -5 /tmp/klf_build_\$fw.log; exit 1; }
done
echo \"built on \$(hostname) at \$(git -C ~/tickle rev-parse --short HEAD)\"" </dev/null &
    build_pids+=($!)
done
build_failed=0
for pid in "${build_pids[@]}"; do wait "$pid" || build_failed=1; done
[ "$build_failed" = 0 ] || { say "BUILD FAILED on at least one Pi - see above. Not measuring."; exit 1; }

# The clock probe, on both Pis; asked on the receiver's test-link address, so the offset is receiver - sender.
for h in "$RPI_CLIENT" "$RPI_SERVER"; do
    ssh_h "$h" "cat > /tmp/klf_probe.py" <"$HERE/clock_offset_probe.py" || { say "probe copy failed"; exit 1; }
done
SERVER_IP=$(ssh_h "$RPI_SERVER" "ip -4 -o addr show dev eth0" </dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
[ -n "$SERVER_IP" ] || { say "REFUSED: no IPv4 address on $RPI_SERVER's eth0 for the clock probe"; exit 1; }
say "clock probe: $RPI_CLIENT -> $SERVER_IP:$PROBE_PORT over eth0, before and after every cross-host run"
probe() { # $1 run id, $2 label
    ssh_h "$RPI_SERVER" "setsid nohup python3 /tmp/klf_probe.py serve --bind $SERVER_IP --port $PROBE_PORT --idle 3 \
        --max 20 > /tmp/klf_probe_srv.log 2>&1 < /dev/null &" </dev/null
    sleep 0.5
    local line
    line=$(ssh_h "$RPI_CLIENT" "python3 /tmp/klf_probe.py query --server $SERVER_IP --port $PROBE_PORT --count 64" \
        </dev/null)
    say "FRESH run=$1 probe=$2 ${line:-PROBE ok=0 no output}"
}

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
    # Rotate the framework order per rep.
    read -r -a fw_arr <<<"$FWS"
    k=$(((rep - 1) % ${#fw_arr[@]}))
    order=("${fw_arr[@]:$k}" "${fw_arr[@]:0:$k}")
    for c in $CELLS; do
        [ "$c" = fault ] && [ "$rep" -gt 1 ] && continue
        IFS='|' read -r topo args <<<"$(cell_spec "$c")"
        for fw in "${order[@]}"; do
            run=$((run + 1))
            left=$(wait_rig_quiet)
            say "FRESH run=$run cell=$c fw=$fw rep=$rep topo=$topo leftover_before=$left args=$args"
            [ "$topo" = cross ] && probe "$run" before
            srv=$RPI_SERVER
            cli=$RPI_CLIENT
            [ "$topo" = same ] && srv=$RPI_CLIENT # rpi#1, the Pi s6_transport_cells.sh's same-host cells run on
            # PRE_CLIENT_SLEEP 0, as comparison_resweep.sh's history cells: the TickLE server stalls before it even
            # creates its subscriber, so a later client would hand it the first samples late.
            # shellcheck disable=SC2086 # args deliberately word-split into the harness argv
            res=$(cd "$PH/$fw" && RPI_CLIENT=$cli RPI_SERVER=$srv PRE_CLIENT_SLEEP=0 timeout 180 \
                ./run_scenario.sh history_depth_burst_loss $args 2>/dev/null | grep '^RESULT:' || true)
            if [ -z "$res" ]; then
                say "FRESH run=$run NO RESULT LINE"
            else
                while IFS= read -r line; do say "FRESH run=$run $line"; done <<<"$res"
            fi
            [ "$topo" = cross ] && probe "$run" after
        done
    done
done
say "=== done $(date -Is): $run runs ==="
say "read with: examples/perf_hil/experiments/freshness_summary.py $OUT"
