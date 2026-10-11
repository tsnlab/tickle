#!/usr/bin/env bash
# hybrid_step_cost.sh - does a hybrid-transport step cost anything where the feature is not used? (DESIGN.md section 8,
# "Hybrid transport", pre-registration row "core P1-P4": no regression by the instruction-count gate with its floor.)
#
# CLAIM: a step's commit costs core P1-P4 nothing against its parent. Neither step 1 (options, announce fields,
# tt_VERSION 13) nor step 2 (receiving runs) changes what a P1-P4 sample does on the wire - nobody sends a run - so any
# difference in work per sample is a cost of the code being there.
#
# ARMS, each a commit (ARM_SHAS="name=sha ..."; the first is the reference). The default is the parent twice, so the
# A/A pair - two builds of one commit, measured in the same session - is in every run, and then each step:
#   parent     b6d23033, the reference
#   parent_aa  the same commit, built again: its difference from parent is the session's noise - the FLOOR
#   step1 ...  the commits under test
# Every arm is built from its own exported tree with the same scenario sources (each tree's own), and the binaries are
# copied out, so nothing can change an arm under the run.
#
# TRAFFIC: cross-host shaped, PC only (no rig, no rig lock). Client and server in two private network namespaces joined
# by a veth pair, each with its own tmpfs on /dev/shm, so every datagram crosses the link. reliable_throughput, RELIABLE
# KEEP_ALL (-Q -N <campaign bound> -B 100, campaign cells 1-4), p1-p4 (p4 is the two-fragment shape), client and server
# pinned. Each process runs under `perf stat -e instructions:u,instructions:k`; the metric is instructions per sample
# (client per sent, server per received), user + kernel - work, which the PC's load does not move.
#
# HOW TO READ IT, written before the first run and implemented in hybrid_step_cost.py (not only here):
#   Run VOID: no single RESULT line from either side; client rc != 0; drained != acked; recv != sent or lost != 0;
#     window != ok; tx_shm or rx_shm != 0 (not cross-host shaped); perf output missing or unreadable; the veth's
#     tx_packets disagreeing with the client's tx_udp by more than 1% + 64.
#   Overall VOID: fewer than REPS_MIN (5) valid runs of any (arm, shape); two arms of the same commit whose clients
#     differ (the build is not reproducible, so A/A is not a control); two arms of different commits whose clients are
#     identical (the change never reached the binary - an arm that silently applies nothing).
#   Per (shape, process), each arm X against the reference, paired by repetition (arm order is rotated per rep and per
#     shape, so drift lands on every arm): d = mean(X/parent - 1), SE over repetitions.
#     FLOOR = max(1%, |d(parent_aa)| + 2 SE(parent_aa)) - the A/A pair's own difference is what this session cannot
#       tell apart; 1% is WIRE 10.4's floor for two builds whose code differs in size.
#     HELD   |d| <= 2 SE.            FLOOR  beyond 2 SE, within FLOOR: not evidence of a cost.
#     COST / BETTER  beyond both, more / fewer instructions.
#   Claim (WIRE 8.3: one COST cell among many is a candidate, not a finding): an arm COSTS iff it is COST in at least
#     half the shapes for one process. Falsification of "no cost where unused": that firing for a step arm.
#   Also printed, not judged: user and kernel instructions apart, and the static sizes per arm and shape -
#   sizeof(struct tt_Context) and of struct tt_Publisher, and text/data/bss of the client.
#
# Usage: hybrid_step_cost.sh      env: ARM_SHAS (see above; default "parent=b6d23033 parent_aa=b6d23033")
#        REPS (6) DUR (4) EDGE_S (1) SHAPES ("p1 p2 p3 p4") CPU_CLIENT (12) CPU_SERVER (13)
#        OUT (default ~/rig_results_safe/hybrid_step_cost/<stamp>) KEEP_WORK=1 keeps the build trees.
# Needs passwordless `sudo -n ip` and perf (perf_event_paranoid <= 1). Long: launch detached and read $OUT/summary.txt.
set -uo pipefail

# ------------------------------------------------------------------------------------------------ one run, as root
if [ "${1:-}" = "--job" ]; then
    mount -t tmpfs -o size=512m tmpfs /dev/shm || {
        echo "note=no_private_shm" >"$J_STATUS"
        exit 1
    }
    events="instructions:u,instructions:k"
    pre_s=(taskset -c "$J_SCPU" perf stat -x "," -e "$events" -o "$J_SPERF" --)
    pre_c=(taskset -c "$J_CCPU" perf stat -x "," -e "$events" -o "$J_CPERF" --)
    # shellcheck disable=SC2016,SC2086 # $1/$@ are the inner sh's; J_ARGS is deliberately word-split into argv
    ip netns exec "$J_SNS" sh -c 'mount -t tmpfs -o size=512m tmpfs /dev/shm && cd "$1" && shift && exec "$@"' \
        sh "$J_DIR" "${pre_s[@]}" env "BENCH_IFACE=$J_SBENCH" ./server $J_ARGS >"$J_SLOG" 2>&1 </dev/null &
    spid=$!
    sleep 1
    tx0=$(cat "/sys/class/net/$J_CIFACE/statistics/tx_packets")
    # shellcheck disable=SC2086 # J_ARGS is deliberately word-split into argv
    (cd "$J_DIR" && exec timeout -s INT -k 5 "$J_CTIMEOUT" "${pre_c[@]}" env "BENCH_IFACE=$J_CBENCH" ./client $J_ARGS) \
        >"$J_CLOG" 2>&1 </dev/null
    crc=$?
    tx1=$(cat "/sys/class/net/$J_CIFACE/statistics/tx_packets")
    # The server is perf's child; confirmed by /proc/PID/exe before the signal, never found by name.
    target=$(awk '{print $1}' "/proc/$spid/task/$spid/children" 2>/dev/null)
    stop=sigint
    case "$(readlink "/proc/${target:-0}/exe" 2>/dev/null)" in
    */server) kill -INT "$target" ;;
    *) stop=gone_before_sigint ;;
    esac
    for _ in $(seq 1 100); do
        kill -0 "$spid" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$spid" 2>/dev/null; then
        stop=killed_after_10s
        [ -n "$target" ] && kill -KILL "$target"
        kill -KILL "$spid"
    fi
    wait "$spid"
    src=$?
    echo "server_rc=$src client_rc=$crc server_stop=$stop veth_tx_packets=$((tx1 - tx0))" >"$J_STATUS"
    exit 0
fi

# ------------------------------------------------------------------------------------------------ the driver
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$HERE/$(basename "${BASH_SOURCE[0]}")"
REPO="$(cd "$HERE/../../.." && pwd)"
REPS=${REPS:-6}
DUR=${DUR:-4}
EDGE_S=${EDGE_S:-1}
SHAPES=${SHAPES:-p1 p2 p3 p4}
ARM_SHAS=${ARM_SHAS:-parent=b6d23033 parent_aa=b6d23033}
CPU_CLIENT=${CPU_CLIENT:-12}
CPU_SERVER=${CPU_SERVER:-13}
SCEN=reliable_throughput
T0=$(date +%s)

declare -A SHA_OF
ARMS=""
for pair in $ARM_SHAS; do
    name=${pair%%=*}
    full=$(git -C "$REPO" rev-parse --verify -q "${pair#*=}^{commit}") || {
        echo "no commit ${pair#*=} in $REPO" >&2
        exit 1
    }
    SHA_OF[$name]=$full
    ARMS="$ARMS $name"
done
ARMS=${ARMS# }
keepall_for() { # campaign_sweep.sh's keepall_samples_for(): these are campaign cells 1-4
    case "$1" in p1) echo 2048 ;; p2) echo 405 ;; p3) echo 368 ;; p4) echo 187 ;; esac
}

OUTD=${OUT:-$HOME/rig_results_safe/hybrid_step_cost/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD/runs" || exit 1
SUMMARY="$OUTD/summary.txt"
: >"$SUMMARY"
say() { echo "$*" | tee -a "$SUMMARY"; }

WORK=$(mktemp -d /tmp/hybrid_step_cost.XXXXXX) || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
# shellcheck disable=SC2329 # invoked by the EXIT trap below
cleanup() {
    local ns pids
    while read -r ns; do
        [ -n "$ns" ] || continue
        pids=$(sudo -n ip netns pids "$ns" 2>/dev/null | tr '\n' ' ')
        # shellcheck disable=SC2086
        [ -n "${pids// /}" ] && sudo -n ip netns exec "$ns" kill -KILL $pids 2>/dev/null
        sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    if [ "${KEEP_WORK:-0}" != 1 ] && [ -n "${WORK:-}" ] && [ -d "$WORK" ]; then rm -rf "$WORK"; fi
    return 0
}
trap cleanup EXIT
trap 'exit 130' INT TERM

say "=== hybrid_step_cost $(date -Is): arms '$ARM_SHAS', shapes '$SHAPES', $REPS reps, -d $DUR, edges ${EDGE_S}s," \
    "cpus client $CPU_CLIENT server $CPU_SERVER ==="
for arm in $ARMS; do say "    arm $arm = ${SHA_OF[$arm]}"; done
say "    output: $OUTD"

# ------------------------------------------------------------------------------------------------ builds
build_arm() { # $1 arm. One exported tree per arm: build.sh cleans its tree, and HOME keeps each arm's prefix private.
    local arm=$1 shape tree log
    tree="$WORK/$arm/src"
    log="$OUTD/build_$arm.log"
    if ! { mkdir -p "$tree" && git -C "$REPO" archive "${SHA_OF[$arm]}" | tar -x -C "$tree"; }; then
        echo "BUILD_FAIL export" >>"$log"
        return 1
    fi
    for shape in $SHAPES; do
        if ! (cd "$tree/examples/perf_hil/tickle" && HOME="$WORK/$arm/home" ./build.sh "$SCEN" "$shape") >>"$log" 2>&1; then
            echo "BUILD_FAIL $shape" >>"$log"
            return 1
        fi
        mkdir -p "$WORK/bin/$arm/$shape"
        cp "$tree/examples/perf_hil/tickle/${SCEN}_$shape/client" "$tree/examples/perf_hil/tickle/${SCEN}_$shape/server" \
            "$WORK/bin/$arm/$shape/" || return 1
    done
    echo "BUILD_OK" >>"$log"
}
for arm in $ARMS; do build_arm "$arm" & done
wait
for arm in $ARMS; do
    tail -1 "$OUTD/build_$arm.log" | grep -q '^BUILD_OK$' || {
        say "FATAL: build of $arm failed - $OUTD/build_$arm.log"
        exit 1
    }
done
say "    built in $(($(date +%s) - T0)) s"

# ------------------------------------------------------------------------------------------------ static
STATIC="$OUTD/static.txt"
: >"$STATIC"
PROBE="$WORK/probe.c"
cat >"$PROBE" <<'EOF'
#include <stdio.h>
#include <tickle/tickle.h>
int main(void) {
    printf("sizeof_context=%zu sizeof_publisher=%zu sizeof_subscriber=%zu frag=%d large=%d\n", sizeof(struct tt_Context),
           sizeof(struct tt_Publisher), sizeof(struct tt_Subscriber), tt_FRAG_ENABLED, tt_LARGE_SAMPLES);
    return 0;
}
EOF
for arm in $ARMS; do
    for shape in $SHAPES; do
        if [ "$shape" = p4 ]; then
            lib=$(find "$WORK/$arm/home" -path '*sample4096*/lib/libtickle.a' | head -1)
        else
            lib=$(find "$WORK/$arm/home" -path '*/lib/libtickle.a' -not -path '*sample4096*' | head -1)
        fi
        pfx=$(dirname "$(dirname "$lib")")
        extra=""
        [ "$shape" = p4 ] && extra="-Dtt_MAX_SAMPLE_LENGTH=4096"
        # shellcheck disable=SC2086 # deliberately word-split
        gcc $extra -I"$pfx/include" "$PROBE" -o "$WORK/probe_${arm}_$shape" || {
            say "FATAL: struct probe did not build for $arm $shape"
            exit 1
        }
        read -r ct cd cb _ < <(size "$WORK/bin/$arm/$shape/client" | tail -1)
        echo "STATIC arm=$arm shape=$shape client_text=$ct client_data=$cd client_bss=$cb" \
            "$("$WORK/probe_${arm}_$shape") client_sha=$(sha256sum "$WORK/bin/$arm/$shape/client" | cut -c1-16)" \
            "sha=${SHA_OF[$arm]:0:8}" >>"$STATIC"
    done
done
cat "$STATIC" >>"$SUMMARY"

# ------------------------------------------------------------------------------------------------ runs
NSID=0
run_one() { # $1 arm, $2 shape, $3 rep
    local arm=$1 shape=$2 rep=$3
    NSID=$((NSID + 1))
    local rd="$OUTD/runs/${shape}_${arm}_r$rep"
    mkdir -p "$rd"
    local nsa="hsc$$-${NSID}a" nsb="hsc$$-${NSID}b" va="hsc$$v${NSID}a" vb="hsc$$v${NSID}b"
    echo "$nsa" >>"$NS_LIST"
    echo "$nsb" >>"$NS_LIST"
    if ! { sudo -n ip netns add "$nsa" && sudo -n ip netns add "$nsb" && sudo -n ip -n "$nsa" link set lo up &&
        sudo -n ip -n "$nsb" link set lo up && sudo -n ip link add "$va" type veth peer name "$vb" &&
        sudo -n ip link set "$va" netns "$nsa" && sudo -n ip link set "$vb" netns "$nsb" &&
        sudo -n ip -n "$nsa" addr add 192.168.10.1/24 brd + dev "$va" &&
        sudo -n ip -n "$nsb" addr add 192.168.10.2/24 brd + dev "$vb" &&
        sudo -n ip -n "$nsa" link set "$va" up && sudo -n ip -n "$nsb" link set "$vb" up; }; then
        echo "note=no_veth" >"$rd/status"
        return
    fi
    local args
    args="-Q -N $(keepall_for "$shape") -B 100 -d $DUR --warmup-s $EDGE_S --cooldown-s $EDGE_S"
    echo "arm=$arm shape=$shape rep=$rep sha=${SHA_OF[$arm]} args='$args' started=$(date -Is)" >"$rd/job"
    # shellcheck disable=SC2024 # the logs are written by this shell, as the invoking user: intended
    sudo -n ip netns exec "$nsa" env J_STATUS="$rd/status" J_CLOG="$rd/client.log" J_SLOG="$rd/server.log" \
        J_DIR="$WORK/bin/$arm/$shape" J_SNS="$nsb" J_CIFACE="$va" J_ARGS="$args" J_CBENCH="$va" J_SBENCH="$vb" \
        J_CCPU="$CPU_CLIENT" J_SCPU="$CPU_SERVER" J_CTIMEOUT=90 J_CPERF="$rd/client.perf" J_SPERF="$rd/server.perf" \
        bash "$SELF" --job >"$rd/job.log" 2>&1
    sudo -n ip netns del "$nsa" 2>/dev/null
    sudo -n ip netns del "$nsb" 2>/dev/null
}

read -r -a ARM_LIST <<<"$ARMS"
NARMS=${#ARM_LIST[@]}
for rep in $(seq 1 "$REPS"); do
    si=0
    for shape in $SHAPES; do
        # Rotated per repetition and per shape, so every arm takes every position in the order as often as possible.
        for k in $(seq 0 $((NARMS - 1))); do
            run_one "${ARM_LIST[$(((k + rep + si) % NARMS))]}" "$shape" "$rep"
        done
        si=$((si + 1))
    done
    say "    rep $rep/$REPS done at $(($(date +%s) - T0)) s"
done

# ------------------------------------------------------------------------------------------------ verdict
python3 "$HERE/hybrid_step_cost.py" "$OUTD" "$ARMS" | tee -a "$SUMMARY"
say "=== done in $(($(date +%s) - T0)) s: $OUTD ==="
