#!/usr/bin/env bash
# tx_gap_pc.sh - the PUBLISHER's cost per MB of a large sample, commit by commit, on the PC: rx_gap_pc.sh's network (two
# private network namespaces, a veth pair, the publisher's egress shaped to 1 Gbit/s by tbf, socket buffers 212,992 B
# as the rig grants), large_sample_bench.c at both ends, built as rmw_tickle builds core. The PUBLISHER runs under perf
# stat (instructions and cycles, user and kernel, task-clock, context switches); with STRACE=1 one extra block runs it
# under `strace -f -c` instead, for its system calls and their errors (EAGAIN), never read as a cost.
#
# Why (2026-10-13): the ab/rx-gap2 rig A/B read the publisher's CPU per MB WORSE in both BEST_EFFORT cells (1 MB +0.084,
# 4 MB +0.031 ms/MB) while both RELIABLE cells held. Each arm here is one commit of that branch (or one of its runtime
# switches), so the step that costs the publisher is named by the arm where its number moves. NOT a rig figure: x86,
# veth, a core bench in one thread (rmw_tickle has its own), never to be tabled as one.
#
# Arms: ARMS="label:commit[:ENV=VALUE,...] ..." - each built from a tree exported with git archive (the working tree is
# never modified), the bench itself always from TIP (the same harness in every arm). The env is given to BOTH ends.
# Cells: CELLS="name:bytes:hz:qflag ...", default be1m and be4m (BEST_EFFORT, as the rig's) and rel1m (RELIABLE: the
# rig held there - the cell the culprit must NOT move, a control of the reading, not of the PC).
#
# HOW TO READ IT - written before the first run (2026-10-13), enforced by tx_gap_pc_read.py:
#   VOID     a run with torn > 0 or out_of_order > 0, a RELIABLE run with delivered < published, a BEST_EFFORT run below
#            98%, no RESULT lines or no perf counts. Fewer than 2 runs of an arm in a cell: that arm not read.
#   METRIC   publisher instructions per MB published, user + kernel (PRIMARY), each half, cycles per MB; beside them the
#            treatment: send_waits, tx_datagrams and runs per MB, n_hist, empty reads, timer fires.
#   FLOOR    per cell, max(|mean(BASE2) - mean(BASE)| (the A/A floor: the first arm built twice, separate runs),
#            2 x SE of the paired step difference).
#   STEP     each arm against the arm before it in ARMS (the branch's order): UP when it rises by more than the floor,
#            DOWN when it falls by more than it, else FLAT.
#   CULPRIT  an arm whose STEP is UP in every BEST_EFFORT cell. None: "NOT LOCALISED on the PC" (the rig's regression is
#            then not a per-instruction cost this PC reproduces). An arm UP in BE whose RELIABLE step is also UP is named
#            but marked "also RELIABLE" (the rig held there: that arm alone does not explain the rig).
# Usage: tx_gap_pc.sh [BLOCKS] [DURATION_S]   ARMS=, CELLS=, OUT= (default ~/largemsg_pc/tx_gap/results.txt). Needs
#        passwordless `sudo ip` and `tc`. Run it detached. Every bench process runs under `ulimit -v` and a timeout.
# The last line is always "tx_gap_pc done rc=<n>".
set -u
BLOCKS=${1:-3}
DURATION=${2:-12}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
TIP=${TIP:-HEAD}
WORK=${WORK:-$HOME/largemsg_pc/tx_gap}
OUT=${OUT:-$WORK/results.txt}
NS1=txg-pub-$$
NS2=txg-sub-$$
VMEM_KB=${VMEM_KB:-2097152}
CELLS=${CELLS:-"be1m:1048576:30: be4m:4194304:15: rel1m:1048576:30:-R"}
ARMS=${ARMS:?ARMS=\"label:commit[:ENV=V,...] ...\" - the first arm is the base, BASE2 is added as its A/A}
CFLAGS_BENCH=${CFLAGS_BENCH:-"-O2 -g -DNDEBUG"}
PUB_FLAGS=${PUB_FLAGS:-"-F auto -M -G"} # the rig's regime: AUTO, the reader's cap on
SUB_FLAGS=${SUB_FLAGS-}
STRACE=${STRACE:-0}
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1

cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "tx_gap_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK/bin" "$TMPDIR"
tip_sha=$(git -C "$TREE" rev-parse --short=8 "$TIP") || exit 1
rm -rf "$WORK/src_tip" && mkdir -p "$WORK/src_tip"
git -C "$TREE" archive "$tip_sha" examples/perf_hil/experiments/large_sample_bench.c | tar -x -C "$WORK/src_tip" || exit 1

declare -A arm_bin arm_env arm_sha
read -r -a arm_specs <<<"$ARMS"
labels=()
for spec in "${arm_specs[@]}"; do
    IFS=: read -r label commit env <<<"$spec"
    sha=$(git -C "$TREE" rev-parse --short=8 "$commit") || exit 1
    if [ ! -x "$WORK/bin/bench_$sha" ]; then
        rm -rf "$WORK/src_$sha" && mkdir -p "$WORK/src_$sha"
        git -C "$TREE" archive "$sha" src include | tar -x -C "$WORK/src_$sha" || exit 1
        # shellcheck disable=SC2086 # CFLAGS_BENCH splits on purpose
        cc $CFLAGS_BENCH -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
            -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$WORK/src_$sha/include" -I"$WORK/src_$sha/src" -o "$WORK/bin/bench_$sha" \
            "$WORK/src_tip/examples/perf_hil/experiments/large_sample_bench.c" "$WORK/src_$sha/src/tickle.c" \
            "$WORK/src_$sha/src/encoding.c" "$WORK/src_$sha/src/log.c" "$WORK/src_$sha/src/hal_linux.c" -lpthread -lm \
            2>"$WORK/build_$sha.log" || { cat "$WORK/build_$sha.log"; exit 1; }
    fi
    arm_bin[$label]=$WORK/bin/bench_$sha
    arm_env[$label]=$(tr ',' ' ' <<<"${env:-}")
    arm_sha[$label]=$sha
    labels+=("$label")
done
base=${labels[0]}
arm_bin[BASE2]=${arm_bin[$base]}
arm_env[BASE2]=${arm_env[$base]}
arm_sha[BASE2]=${arm_sha[$base]}
labels+=(BASE2)

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add txga netns "$NS1" type veth peer name txgb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.87.1/24 dev txga
sudo -n ip -n "$NS2" addr add 192.168.87.2/24 dev txgb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set txga up
sudo -n ip -n "$NS2" link set txgb up
sudo -n ip netns exec "$NS1" tc qdisc add dev txga root tbf rate 1gbit burst 64kb latency 100ms || exit 1
arms_line=""
for l in "${labels[@]}"; do arms_line+=" $l=${arm_sha[$l]}${arm_env[$l]:+[${arm_env[$l]}]}"; done
echo "=== tx_gap_pc $(date -Is) tip=$tip_sha arms:$arms_line pub_flags='$PUB_FLAGS' sub_flags='$SUB_FLAGS' blocks=$BLOCKS duration=${DURATION}s strace=$STRACE cflags='$CFLAGS_BENCH' load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

run() { # <cell> <arm> <block> <size> <rate> <qflag> <mode: perf|strace>
    local cell=$1 arm=$2 block=$3 size=$4 rate=$5 qflag=$6 mode=$7
    local bin=${arm_bin[$arm]} envs=${arm_env[$arm]}
    local sublog=$WORK/sub.log publog=$WORK/pub.log perflog=$WORK/perf.txt stlog=$WORK/strace.txt wrap
    rm -f "$perflog" "$stlog"
    if [ "$mode" = strace ]; then
        wrap="strace -f -c -o $stlog"
    else
        wrap="perf stat -x, -e instructions:u,instructions:k,cycles:u,cycles:k,task-clock,context-switches -o $perflog"
    fi
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 40)) env $envs $bin sub \
        -s $size -d $((DURATION + 8)) $qflag $SUB_FLAGS -b 192.168.87.255" >"$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $wrap env $envs $bin pub \
        -s $size -r $rate -d $DURATION $qflag $PUB_FLAGS -b 192.168.87.255" >"$publog" 2>&1
    wait "$sub_pid"
    local p s iu ik cu ck tc cs rx st
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "RUN cell=$cell arm=$arm block=$block mode=$mode VOID no RESULT (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
        return
    fi
    rx=$(grep -m1 -o 'rx: .*' "$publog" | tr ' ' '\n' |
        grep -E '^(empty_reads|timer_[a-z]+|nonblock|direct_reads)=' | sed 's/^/pub_/' | tr '\n' ' ')
    if [ "$mode" = strace ]; then
        # "calls errors syscall" of every call that failed at least once, and the calls of the send and wait ones
        st=$(awk '$NF ~ /^(sendmmsg|sendmsg|sendto|poll|ppoll|epoll_pwait|epoll_pwait2|epoll_wait|timerfd_settime|read|recvmsg|recvfrom|recvmmsg|io_uring_enter)$/ {
                  if (NF == 6) printf "sc_%s=%s/%s ", $NF, $4, $5; else printf "sc_%s=%s/0 ", $NF, $4 }' "$stlog")
        echo "RUN cell=$cell arm=$arm block=$block mode=strace $st$rx| $p | $s" >>"$OUT"
        echo "RUN cell=$cell arm=$arm block=$block mode=strace $st"
        return
    fi
    iu=$(grep -m1 'instructions:u' "$perflog" 2>/dev/null | cut -d, -f1)
    ik=$(grep -m1 'instructions:k' "$perflog" 2>/dev/null | cut -d, -f1)
    cu=$(grep -m1 'cycles:u' "$perflog" 2>/dev/null | cut -d, -f1)
    ck=$(grep -m1 'cycles:k' "$perflog" 2>/dev/null | cut -d, -f1)
    tc=$(grep -m1 'task-clock' "$perflog" 2>/dev/null | cut -d, -f1)
    cs=$(grep -m1 'context-switches' "$perflog" 2>/dev/null | cut -d, -f1)
    echo "RUN cell=$cell arm=$arm block=$block mode=perf insn_u=${iu:-none} insn_k=${ik:-none} cyc_u=${cu:-none} cyc_k=${ck:-none} task_ms=${tc:-none} ctx=${cs:-none} $rx| $p | $s" >>"$OUT"
    echo "RUN cell=$cell arm=$arm block=$block insn_u=${iu:-none} insn_k=${ik:-none} $(grep -oE '(published|large_send_waits)=[0-9]*' <<<"$p" | tr '\n' ' ')$(grep -o 'delivered=[0-9]*' <<<"$s") $rx"
}

# Arms rotate per block (each block starts one arm later), cells in turn.
n=${#labels[@]}
for block in $(seq 1 "$BLOCKS"); do
    for spec in $CELLS; do
        IFS=: read -r cell size rate qflag <<<"$spec"
        for i in $(seq 0 $((n - 1))); do
            run "$cell" "${labels[$(((i + block - 1) % n))]}" "$block" "$size" "$rate" "$qflag" perf
        done
    done
done
if [ "$STRACE" = 1 ]; then
    for spec in $CELLS; do
        IFS=: read -r cell size rate qflag <<<"$spec"
        for l in "${labels[@]}"; do
            [ "$l" = BASE2 ] || run "$cell" "$l" s "$size" "$rate" "$qflag" strace
        done
    done
fi
echo "=== done $(date -Is) ===" | tee -a "$OUT"
python3 "$HERE/tx_gap_pc_read.py" "$OUT" | tee -a "$OUT.read"
rc=${PIPESTATUS[0]}
