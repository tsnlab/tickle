#!/usr/bin/env bash
# srv_gap_pc.sh - the core campaign cells' SERVER and client, commit by commit, on the PC: executed instructions (user
# and kernel, perf stat over each process's whole life, as gate v2 counts them on the rig) per sample. Two private
# network namespaces over a veth, 192.168.10.0/24 (the bench's broadcast). lat_gap_pc.sh counted the latency client
# only; this counts both roles, in a latency cell and in the throughput cells, so a server-side step can be named.
#
# Why (2026-10-14): the ab/rx-gap3 rig gate v2 (A = 481b9e3e, B = 32d999ca, core p1-p4, A P B B P A) read the SERVER's
# insn_u_per_sample +0.16..+0.74% in every throughput cell (+6..+15 a sample) and every latency cell (+25..+29 a round
# trip), t 7..98, while the client fell (kernel -20%, CPU -13%: the intended fix). NOT a rig figure (x86, veth).
#
# The cells are the rig's, with the rig's arguments (campaign_sweep.sh: cell_args below), the server started 3 s
# before the client and SIGINTed when the client returns (tickle/run_scenario.sh), so a whole-life count covers what
# the rig's does: L = reliable_latency (c10 p1, c17 p4), T = reliable_throughput -Q KEEP_ALL (Q0, c2 p2) and
# best_effort_throughput (Q1, c13 p2). Per sample = the process's own sent= (client) or recv= (throughput server); the
# latency server divides by its client's sent= (campaign_summary.py, INSN_METRICS).
#
# Builds as the gate does: every arm with TICKLE_LAYOUT_CFLAGS (gate_v2_lib.sh A1); an arm "label:commit:P" with
# TICKLE_PLACEMENT_CFLAGS (A3, -falign-functions=128), the placement arm.
#
# HOW TO READ IT - written before the first run, and done by srv_gap_pc_read.py (which implements it):
#   - The first arm is run twice (BASE2, its own blocks in the rotation): |BASE2 - first| per (cell, role, metric) is
#     the A/A floor.
#   - A step (arm vs the arm named before it on the reader's STEPS line) MOVED when |d| > max(2 x A/A floor,
#     3 x SE(d), 1 instruction). The steps between A and B whose server insn_u MOVED up name the cause; their sum must
#     come within 3 SE of B - A (else "NOT ADDITIVE": something between the arms is not a step).
#   - Layout: B - A against PB - PA (both placement builds). The delta "SURVIVES LAYOUT" when both MOVED the same way
#     and differ by less than half of B - A; "LAYOUT" when PB - PA did not move; anything else "MIXED".
#   - VOID: a run without both RESULT lines, without both counts, or with 0 samples; an arm with fewer than 2 valid
#     blocks in a cell is not read there.
# Usage: srv_gap_pc.sh [BLOCKS]   ARMS="label:commit[:P] ...", CELLS (default "L:p1 T:p2:Q0 T:p2:Q1 L:p4"),
# STRACE=1 (one extra server pass per arm and cell under strace -f -c), WORK= (default ~/largemsg_pc/rx_gap3_server),
# OUT= (default $WORK/results.txt), TREE=. Run it detached. Every bench process runs under ulimit -v and a timeout.
# The last line is always "srv_gap_pc done rc=<n>".
set -u
BLOCKS=${1:-3}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_pc/rx_gap3_server}
OUT=${OUT:-$WORK/results.txt}
ARMS=${ARMS:?ARMS=\"label:commit[:P] ...\"}
CELLS=${CELLS:-L:p1 T:p2:Q0 T:p2:Q1 L:p4}
VMEM_KB=${VMEM_KB:-2097152}
PRE_CLIENT_SLEEP=${PRE_CLIENT_SLEEP:-3}
BUILD_JOBS=${BUILD_JOBS:-4}
NS1=sg-cli-$$
NS2=sg-srv-$$
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091
. "$HERE/gate_v2_lib.sh"
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "srv_gap_pc done rc=$rc $(date -Is)" | tee -a "$OUT"
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK" "$TMPDIR"

scenario_of() { # <cell>: the scenario directory build.sh fills
    IFS=: read -r shape payload qos <<<"$1"
    if [ "$shape" = L ]; then echo "reliable_latency_$payload"
    elif [ "$qos" = Q1 ]; then echo "best_effort_throughput_$payload"
    else echo "reliable_throughput_$payload"; fi
}
cell_args() { # <cell>: campaign_sweep.sh's arguments for it (TickLE), to both roles
    IFS=: read -r shape payload qos <<<"$1"
    if [ "$shape" = L ]; then echo "-i 0.1 -d 10 -W 4096 -C 4096 -I 0.001 -T 20"; return; fi
    local keep
    case "$payload" in p1) keep=2048 ;; p2) keep=405 ;; p3) keep=368 ;; *) keep=187 ;; esac
    if [ "$qos" = Q1 ]; then echo "-d 5 --warmup-s 2 --cooldown-s 2"
    else echo "-Q -N $keep -B 100 -d 5 --warmup-s 2 --cooldown-s 2"; fi
}

declare -A arm_dir arm_sha arm_flavor
labels=()
builds=()
for spec in $ARMS; do
    IFS=: read -r label commit flavor <<<"$spec"
    sha=$(git -C "$TREE" rev-parse --short=8 "$commit") || exit 1
    flavor=${flavor:-L}
    d=$WORK/src_${sha}_$flavor
    arm_dir[$label]=$d/examples/perf_hil/tickle
    arm_sha[$label]=$sha
    arm_flavor[$label]=$flavor
    labels+=("$label")
    case " ${builds[*]} " in *" $sha:$flavor "*) ;; *) builds+=("$sha:$flavor") ;; esac
done

build_one() { # <sha> <flavor>: every cell's scenario, into its own source copy (own HOME: own install prefixes)
    local sha=$1 flavor=$2 d=$WORK/src_${1}_$2 cell flags
    if [ "$flavor" = P ]; then flags=$TICKLE_PLACEMENT_CFLAGS; else flags=$TICKLE_LAYOUT_CFLAGS; fi
    if [ ! -f "$d/.built" ]; then
        rm -rf "$d" && mkdir -p "$d/home"
        git -C "$TREE" archive "$sha" | tar -x -C "$d" || return 1
    fi
    for cell in $CELLS; do
        local scen
        scen=$(scenario_of "$cell")
        [ -x "$d/examples/perf_hil/tickle/$scen/server" ] && continue
        (cd "$d/examples/perf_hil/tickle" && HOME=$d/home TICKLE_EXTRA_CFLAGS="$flags" \
            ./build.sh "${scen%_p?}" "${scen##*_}") >>"$WORK/build_${sha}_$flavor.log" 2>&1 || return 1
    done
    touch "$d/.built"
}
echo "building ${builds[*]} ($(date -Is))"
pids=()
for b in "${builds[@]}"; do
    while [ "$(jobs -rp | wc -l)" -ge "$BUILD_JOBS" ]; do sleep 2; done
    build_one "${b%%:*}" "${b##*:}" &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p" || { echo "BUILD FAILED (see $WORK/build_*.log)"; exit 1; }; done
echo "built ($(date -Is))"

arm_dir[BASE2]=${arm_dir[${labels[0]}]}
arm_sha[BASE2]=${arm_sha[${labels[0]}]}
arm_flavor[BASE2]=${arm_flavor[${labels[0]}]}
labels+=(BASE2)

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add sga netns "$NS1" type veth peer name sgb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev sga
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev sgb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set sga up
sudo -n ip -n "$NS2" link set sgb up
line="=== srv_gap_pc $(date -Is) arms:"
for l in "${labels[@]}"; do line+=" $l=${arm_sha[$l]}:${arm_flavor[$l]}"; done
echo "$line cells=[$CELLS] blocks=$BLOCKS load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

count() { # <perf -x, file> <event>
    awk -F, -v e="$2" '$3 ~ "^"e && $1 ~ /^[0-9]+$/ { print $1; exit }' "$1" 2>/dev/null
}

run() { # <arm> <cell> <block> <mode: perf|strace>
    local arm=$1 cell=$2 block=$3 mode=$4 scen args dir
    scen=$(scenario_of "$cell")
    args=$(cell_args "$cell")
    dir=${arm_dir[$arm]}/$scen
    local slog=$WORK/srv.log clog=$WORK/cli.log smeas=$WORK/srv_meas.txt cmeas=$WORK/cli_meas.txt flag=$WORK/client_done
    rm -f "$smeas" "$cmeas" "$flag" "$slog" "$clog"
    local swrap
    if [ "$mode" = strace ]; then swrap="strace -f -c -o $smeas"
    else swrap="perf stat -x, -o $smeas -e $INSN_EVENTS --"; fi
    # The server: its counter is perf's (or strace's) child, timeout, which forwards the SIGINT sent when the client is
    # done (run_scenario.sh's stop) to the server. The PID is the launch's own ($!), its child read from /proc and
    # checked by exe before the signal.
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; cd $dir || exit 1
        $swrap timeout -s INT 90 ./server $args >$slog 2>&1 &
        p=\$!
        until [ -e $flag ] || ! kill -0 \$p 2>/dev/null; do sleep 0.05; done
        for c in \$(cat /proc/\$p/task/*/children 2>/dev/null); do
            case \$(readlink /proc/\$c/exe) in */timeout) kill -INT \$c ;; esac
        done
        wait \$p" &
    local spid=$!
    sleep "$PRE_CLIENT_SLEEP"
    local cwrap="perf stat -x, -o $cmeas -e $INSN_EVENTS --"
    [ "$mode" = strace ] && cwrap=""
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; cd $dir && exec $cwrap timeout -s INT 80 ./client $args" \
        >"$clog" 2>&1
    touch "$flag"
    wait "$spid"
    local cres sres sent recv div
    cres=$(grep -m1 '^RESULT' "$clog")
    sres=$(grep -m1 '^RESULT' "$slog")
    sent=$(grep -o ' sent=[0-9]*' <<<"$cres" | head -1 | cut -d= -f2)
    recv=$(grep -o ' recv=[0-9]*' <<<"$sres" | head -1 | cut -d= -f2)
    if [ "${cell%%:*}" = L ]; then div=$sent; else div=$recv; fi
    if [ -z "$cres" ] || [ -z "$sres" ] || [ -z "$sent" ] || [ "$sent" = 0 ] || [ -z "$div" ] || [ "$div" = 0 ]; then
        echo "RUN arm=$arm cell=$cell block=$block mode=$mode VOID (client: $(tail -1 "$clog" | cut -c1-200)) (server: $(tail -1 "$slog" | cut -c1-200))" | tee -a "$OUT"
        return
    fi
    if [ "$mode" = strace ]; then
        local sc
        sc=$(awk -v n="$div" '$NF != "total" && $NF ~ /^[a-z_0-9]+$/ && NF >= 5 {
                calls = $4; err = (NF == 6) ? $5 : 0; if (calls + 0 > 0) printf "%s=%.3f/%.3f ", $NF, calls / n, err / n }' "$smeas")
        echo "RUN arm=$arm cell=$cell block=$block mode=strace role=server samples=$div per_sample: $sc" >>"$OUT"
        echo "RUN arm=$arm cell=$cell block=$block mode=strace samples=$div"
        return
    fi
    local siu sik ciu cik
    siu=$(count "$smeas" instructions:u); sik=$(count "$smeas" instructions:k)
    ciu=$(count "$cmeas" instructions:u); cik=$(count "$cmeas" instructions:k)
    if [ -z "$siu" ] || [ -z "$sik" ] || [ -z "$ciu" ] || [ -z "$cik" ]; then
        echo "RUN arm=$arm cell=$cell block=$block mode=perf VOID (counts: srv '$siu' '$sik' cli '$ciu' '$cik')" | tee -a "$OUT"
        return
    fi
    echo "RUN arm=$arm sha=${arm_sha[$arm]} flavor=${arm_flavor[$arm]} cell=$cell block=$block mode=perf client_sent=$sent server_samples=$div srv_insn_u=$siu srv_insn_k=$sik cli_insn_u=$ciu cli_insn_k=$cik | C $cres | S $sres | SRX $(grep -m1 ' rx: ' "$slog" | sed 's/.* rx: //')" >>"$OUT"
    echo "RUN arm=$arm cell=$cell block=$block srv_u/s=$((siu / div)) srv_k/s=$((sik / div)) cli_u/s=$((ciu / sent)) cli_k/s=$((cik / sent))"
}

n=${#labels[@]}
for block in $(seq 1 "$BLOCKS"); do
    for cell in $CELLS; do
        for i in $(seq 0 $((n - 1))); do
            run "${labels[$(((i + block - 1) % n))]}" "$cell" "$block" perf
        done
    done
done
if [ "${STRACE:-0}" = 1 ]; then
    for cell in $CELLS; do
        for l in "${labels[@]}"; do [ "$l" = BASE2 ] || run "$l" "$cell" s strace; done
    done
fi
echo "=== done $(date -Is) ===" | tee -a "$OUT"
rc=0
