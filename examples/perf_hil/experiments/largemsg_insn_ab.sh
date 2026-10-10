#!/usr/bin/env bash
# largemsg_insn_ab.sh - publisher instructions per sample and peak RSS of several builds of the core bench, on this PC,
# cross-host shaped (two private network namespaces joined by a veth pair, each process with its own /dev/shm).
#
# WHY (2026-10-10): the rig's A/B of large-message step A rebased onto main (largemsgL_l2x, A = 2e9e861c, B = a5747a85)
# read client cpu_s_per_Msample +2.0% at p2/p3 and client peak RSS +15 kB at p1, where the same step A on its old base
# (dc056907 vs 2940410c) had held 80/80. This finds the commit and the code by counting work, not time: the PC is loaded
# by other sessions, and an instruction count does not move with load or clock.
#
# ARMS="label=<commit> ..." (default below): each arm is a commit exported with git archive, or label=@<dir> for a tree
# in place (a working tree with a candidate fix). Built exactly as the rig builds them (examples/perf_hil/tickle/
# build.sh: libtickle.a release -O2 -DNDEBUG, the bench -O2, the default tt_ config; p4 with tt_MAX_SAMPLE_LENGTH 4096),
# with HOME inside the work directory so the core prefixes are private to this run. reliable_throughput -Q -N <the
# campaign bound> -B 100 (campaign cells c1-c4), client and server pinned. Each run goes through stage1_payg.sh --job,
# the same runner as stage 1's harness (perf stat -e instructions:u,instructions:k as the parent of each bench process,
# the RSS split preload).
#
# HOW TO READ IT, written before the first run and implemented in largemsg_insn_ab.py:
#   Run VOID: no single RESULT line from either side; client rc != 0; drained != acked; recv != sent or lost != 0;
#     window != ok; tx_shm/rx_shm != 0 (not cross-host shaped); no perf counts.
#   The CONTROL is arm A2, a second export and build of A's commit: A2 vs A measures what this PC and this instrument
#     call a difference between two identical builds. Its 2 SE and |d| set the floor for every other comparison.
#   Per (shape, metric), X vs A paired by repetition (arms rotated inside each repetition): d = mean(X/A - 1), SE over
#     repetitions. DIFFERS when |d| > 2 SE AND |d| > FLOOR = max(0.3%, |d(A2,A)| + 2 SE(A2,A)) for instructions;
#     for peak RSS FLOOR = max(4 kB, |d(A2,A)| + 2 SE(A2,A)) in kB. Otherwise SAME.
#   The bisect reading: the first commit in history order that DIFFERS from A on client user instructions at p3 is the
#     commit responsible; if none differs, the PC does not reproduce the rig's finding and the cause is not found here.
#   The fix holds when the fixed arm is SAME as A on client instructions at p1-p4, and its own tests pass.
#   Falsification of "the old pair is clean": OB (dc056907) DIFFERS from O (2940410c) on client instructions at p3.
#
# Usage: largemsg_insn_ab.sh   env: ARMS REPS (4) DUR (3) EDGE_S (1) SHAPES ("p1 p3") CPU_CLIENT (12) CPU_SERVER (13)
#        OUT (default ~/rig_results_safe/largemsg_insn/<stamp>)
# PC only: no rig, no rig lock. Needs passwordless `sudo -n ip`. Long: launch detached (setsid nohup) and read $OUT.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
RUNNER="$HERE/stage1_payg.sh"
ARMS=${ARMS:-A=2e9e861c A2=2e9e861c m304=304f8751 m426=426279ad B=a5747a85 O=2940410c OB=dc056907}
REPS=${REPS:-4}
DUR=${DUR:-3}
EDGE_S=${EDGE_S:-1}
SHAPES=${SHAPES:-p1 p3}
CPU_CLIENT=${CPU_CLIENT:-12}
CPU_SERVER=${CPU_SERVER:-13}
SCEN=reliable_throughput
T0=$(date +%s)
keepall_for() { case "$1" in p1) echo 2048 ;; p2) echo 405 ;; p3) echo 368 ;; p4) echo 187 ;; esac; }

OUTD=${OUT:-$HOME/rig_results_safe/largemsg_insn/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD/runs" || exit 1
SUMMARY="$OUTD/summary.txt"
: >"$SUMMARY"
say() { echo "$*" | tee -a "$SUMMARY"; }

WORK=$(mktemp -d /tmp/largemsg_insn.XXXXXX) || exit 1
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

LABELS=()
for spec in $ARMS; do LABELS+=("${spec%%=*}"); done
say "=== largemsg_insn_ab $(date -Is): arms '$ARMS', shapes '$SHAPES', $REPS reps, -d $DUR, edges ${EDGE_S}s ==="
say "    output: $OUTD"
PRELOAD="$WORK/rss_split_preload.so"
gcc -O2 -Wall -Wextra -shared -fPIC -o "$PRELOAD" "$HERE/rss_split_preload.c" || { say "FATAL: rss_split_preload"; exit 1; }

# ------------------------------------------------------------------------------------------------ builds
build_arm() { # $1 label, $2 commit or @dir
    local label=$1 src=$2 tree="$WORK/$1/src" log="$OUTD/build_$1.log" shape
    mkdir -p "$tree"
    if [ "${src:0:1}" = @ ]; then
        (cd "${src:1}" && git ls-files -z | tar --null -T - -cf -) | tar -x -C "$tree"
        echo "source: tree ${src:1} at $(git -C "${src:1}" rev-parse HEAD) + $(git -C "${src:1}" diff HEAD | sha256sum | cut -c1-12)" >"$log"
    else
        git -C "$REPO" archive "$src" | tar -x -C "$tree"
        echo "source: commit $(git -C "$REPO" rev-parse "$src")" >"$log"
    fi
    for shape in $SHAPES; do
        (cd "$tree/examples/perf_hil/tickle" && HOME="$WORK/$label/home" ./build.sh "$SCEN" "$shape") >>"$log" 2>&1 ||
            { echo "BUILD_FAIL $shape" >>"$log"; return 1; }
        mkdir -p "$WORK/bin/$label/$shape"
        cp "$tree/examples/perf_hil/tickle/${SCEN}_$shape/client" "$tree/examples/perf_hil/tickle/${SCEN}_$shape/server" \
            "$WORK/bin/$label/$shape/" || return 1
    done
    echo "BUILD_OK" >>"$log"
}
for spec in $ARMS; do build_arm "${spec%%=*}" "${spec#*=}" & done
wait
for l in "${LABELS[@]}"; do
    tail -1 "$OUTD/build_$l.log" | grep -q '^BUILD_OK$' || { say "FATAL: build of $l failed - $OUTD/build_$l.log"; exit 1; }
done
say "    built in $(($(date +%s) - T0)) s"
PROBE="$WORK/probe.c"
cat >"$PROBE" <<'EOF'
#include <stdio.h>
#include <tickle/tickle.h>
#ifndef tt_LARGE_SAMPLES
#define tt_LARGE_SAMPLES (-1)
#endif
int main(void) {
    printf("sizeof_context=%zu sizeof_publisher=%zu sizeof_subscriber=%zu large=%d\n", sizeof(struct tt_Context),
           sizeof(struct tt_Publisher), sizeof(struct tt_Subscriber), (int)tt_LARGE_SAMPLES);
    return 0;
}
EOF
for l in "${LABELS[@]}"; do
    for shape in $SHAPES; do
        extra="" lib=""
        if [ "$shape" = p4 ]; then
            extra="-Dtt_MAX_SAMPLE_LENGTH=4096"
            lib=$(find "$WORK/$l/home" -path '*sample4096*/lib/libtickle.a' | head -1)
        else
            lib=$(find "$WORK/$l/home" -path '*/lib/libtickle.a' -not -path '*sample4096*' | head -1)
        fi
        pfx=$(dirname "$(dirname "$lib")")
        # shellcheck disable=SC2086 # extra is deliberately word-split
        sz=$(gcc -DNDEBUG $extra -I"$pfx/include" "$PROBE" -o "$WORK/probe_$l" && "$WORK/probe_$l")
        read -r ct cd cb _ < <(size "$WORK/bin/$l/$shape/client" | tail -1)
        say "STATIC arm=$l shape=$shape client_text=$ct client_data=$cd client_bss=$cb $sz client_sha=$(sha256sum "$WORK/bin/$l/$shape/client" | cut -c1-16)"
    done
done

# ------------------------------------------------------------------------------------------------ runs
NSID=0
run_one() { # $1 arm, $2 shape, $3 rep
    local arm=$1 shape=$2 rep=$3
    NSID=$((NSID + 1))
    local rd="$OUTD/runs/${shape}_${arm}_r${rep}"
    mkdir -p "$rd"
    local nsa="lmi$$-${NSID}a" nsb="lmi$$-${NSID}b" va="lmi$$v${NSID}a" vb="lmi$$v${NSID}b"
    echo "$nsa" >>"$NS_LIST"
    echo "$nsb" >>"$NS_LIST"
    if ! { sudo -n ip netns add "$nsa" && sudo -n ip -n "$nsa" link set lo up && sudo -n ip netns add "$nsb" &&
        sudo -n ip link add "$va" type veth peer name "$vb" && sudo -n ip link set "$va" netns "$nsa" &&
        sudo -n ip link set "$vb" netns "$nsb" && sudo -n ip -n "$nsa" addr add 192.168.10.1/24 brd + dev "$va" &&
        sudo -n ip -n "$nsb" addr add 192.168.10.2/24 brd + dev "$vb" && sudo -n ip -n "$nsb" link set lo up &&
        sudo -n ip -n "$nsa" link set "$va" up && sudo -n ip -n "$nsb" link set "$vb" up; }; then
        echo "note=no_veth" >"$rd/status"
        return
    fi
    local args
    args="-Q -N $(keepall_for "$shape") -B 100 -d $DUR --warmup-s $EDGE_S --cooldown-s $EDGE_S"
    echo "arm=$arm shape=$shape rep=$rep topo=veth args='$args' started=$(date -Is)" >"$rd/job"
    # shellcheck disable=SC2024 # the logs are written by this shell, as the invoking user: intended
    sudo -n ip netns exec "$nsa" env J_STATUS="$rd/status" J_CLOG="$rd/client.log" J_SLOG="$rd/server.log" \
        J_DIR="$WORK/bin/$arm/$shape" J_SNS="$nsb" J_CIFACE="$va" J_ARGS="$args" J_CBENCH="$va" \
        J_SBENCH="$vb" J_CCPU="$CPU_CLIENT" J_SCPU="$CPU_SERVER" J_CTIMEOUT=90 J_PERF=1 \
        J_CPERF="$rd/client.perf" J_SPERF="$rd/server.perf" J_PRELOAD="$PRELOAD" \
        bash "$RUNNER" --job >"$rd/job.log" 2>&1
    sudo -n ip netns del "$nsa" 2>/dev/null
    sudo -n ip netns del "$nsb" 2>/dev/null
}
N=${#LABELS[@]}
for rep in $(seq 1 "$REPS"); do
    si=0
    for shape in $SHAPES; do
        for k in $(seq 0 $((N - 1))); do
            run_one "${LABELS[$(((k + rep + si) % N))]}" "$shape" "$rep"
        done
        si=$((si + 1))
    done
    say "    rep $rep/$REPS done at $(($(date +%s) - T0)) s"
done
python3 "$HERE/largemsg_insn_ab.py" "$OUTD" "${LABELS[@]}" | tee -a "$SUMMARY"
say "=== done in $(($(date +%s) - T0)) s: $OUTD ==="
