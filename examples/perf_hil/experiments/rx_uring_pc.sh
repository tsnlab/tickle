#!/usr/bin/env bash
# rx_uring_pc.sh - tt_receive()'s wait on io_uring (ab/rx-uring-wait, ~/claude_reports/RX_URING_WAIT_DESIGN_2026-10-13.md)
# against its control on the PC, before the rig sees it. Two parts, in private network namespaces only:
#   LARGE  the SUBSCRIBER's cost per MB delivered: tx_gap_pc.sh's network (a veth pair, the publisher's egress shaped to
#          1 Gbit/s by tbf, socket buffers 212,992 B as the rig grants), large_sample_bench.c at both ends built as
#          rmw_tickle builds core, the rig's regime (-F auto -M -G: hybrid runs, the reader's cap on). The subscriber
#          runs under perf stat (instructions and cycles, user and kernel) and, one block more, under strace -f -c
#          (system calls by name). Cells be1m/rel1m (1 MiB 30 Hz) and be4m/rel4m (4 MiB 15 Hz), BEST_EFFORT and RELIABLE.
#   PING   the core reliable_latency ping-pong's CLIENT per round trip (lat_gap_pc.sh, unchanged): instructions user and
#          kernel, and system calls by name. ab/rx-gap2 was rejected for +22% client kernel instructions a round trip.
# Arms: ARMS="label:commit[:ENV=V,...] ..." (default: A = TIP with TT_RX_URING_WAIT=0, the control, the same binary; B = TIP;
# BASE = 32d999ca, the branch's parent). The env goes to the SUBSCRIBER only in LARGE (the publisher is the same in every
# arm: TIP, the ring on) and to both ends in PING (lat_gap_pc.sh's rule). The first arm is run twice (BASE2: A/A). More
# arms (one per commit of the branch, say) are shown against A; only B, and BASE for PING, decide.
# NOT a rig figure and never to be tabled as one: x86, veth, PC kernel.
#
# HOW TO READ IT - written before the first run (2026-10-14), enforced by rx_uring_pc_read.py:
#   VOID     LARGE: a run with torn > 0 or out_of_order > 0, a RELIABLE run with delivered < published, a BEST_EFFORT run
#            below 98%, no RESULT lines or no perf counts. PING: a run without RESULT or sent = 0. Fewer than 2 runs of
#            an arm: that arm not read.
#   FLOOR    per cell and metric: max(|mean(A/A) - mean(A)|, 2 x SE of the paired B - A difference).
#   LARGE    PRIMARY subscriber instructions per MB delivered (user + kernel); beside it each half and, from strace,
#            system calls per MB by name and in total. Per cell: BETTER when B < A by more than the floor, WORSE when
#            B > A by more than it, else HELD.
#            MECHANISM (else the cell is VOID for the claim, not a loss): B's strace shows io_uring_enter per MB > 0 and
#            its epoll_pwait2 + recvfrom + recvmsg per MB at most 10% of A's - the design's rule. A commit that keeps
#            some reads on the epoll path by design (C1: hinted runs; C1-C2: receive offload) is reported ENGAGED-PART
#            with the share, read but not claimed.
#   PING     PRIMARY client kernel instructions per round trip; beside it user instructions and system calls per round
#            trip. REGRESSION when B - A > floor on kernel OR user instructions a round trip, or when B makes more
#            system calls a round trip than A. The same rule B against BASE (the branch must not regress against its
#            parent, 32d999ca). Expected (not a criterion): one system call fewer a wake on the ring.
#   VERDICT  The change is supported on the PC when no PING regression (B vs A and B vs BASE) and no LARGE cell WORSE on
#            PRIMARY. It is refuted on the PC by a PING regression, or by WORSE on PRIMARY in both RELIABLE cells.
# Usage: rx_uring_pc.sh [BLOCKS] [DURATION_S]   ARMS=, CELLS=, PARTS="large ping", OUT= (default
#        ~/largemsg_pc/rx_uring/results.txt; PING's go to <OUT>.ping). Needs passwordless `sudo ip` and `tc`. Run it
#        detached. Every bench process runs under `ulimit -v` and a timeout. The last line is always
#        "rx_uring_pc done rc=<n>".
set -u
BLOCKS=${1:-3}
DURATION=${2:-12}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
TIP=${TIP:-HEAD}
WORK=${WORK:-$HOME/largemsg_pc/rx_uring}
OUT=${OUT:-$WORK/results.txt}
NS1=rxu-pub-$$
NS2=rxu-sub-$$
VMEM_KB=${VMEM_KB:-2097152}
CELLS=${CELLS:-"be1m:1048576:30: rel1m:1048576:30:-R be4m:4194304:15: rel4m:4194304:15:-R"}
ARMS=${ARMS:-"A:$TIP:TT_RX_URING_WAIT=0 B:$TIP BASE:32d999ca"}
PARTS=${PARTS:-"large ping"}
CFLAGS_BENCH=${CFLAGS_BENCH:-"-O2 -g -DNDEBUG"}
PUB_FLAGS=${PUB_FLAGS:-"-F auto -M -G"} # the rig's regime: AUTO, the reader's cap on
SUB_FLAGS=${SUB_FLAGS--K 64} # every cache line checked, not every byte (rx_gap_pc.sh: every byte buried the receive path)
PING_BLOCKS=${PING_BLOCKS:-3}
PING_SECS=${PING_SECS:-5}
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1

cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "rx_uring_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK/bin" "$TMPDIR"
tip_sha=$(git -C "$TREE" rev-parse --short=8 "$TIP") || exit 1
rm -rf "$WORK/src_tip" && mkdir -p "$WORK/src_tip"
git -C "$TREE" archive "$tip_sha" examples/perf_hil/experiments/large_sample_bench.c | tar -x -C "$WORK/src_tip" || exit 1

build_bench() { # <sha>: the bench, core from <sha>, the harness from TIP
    local sha=$1
    [ -x "$WORK/bin/bench_$sha" ] && return 0
    rm -rf "$WORK/src_$sha" && mkdir -p "$WORK/src_$sha"
    git -C "$TREE" archive "$sha" src include | tar -x -C "$WORK/src_$sha" || return 1
    # shellcheck disable=SC2086 # CFLAGS_BENCH splits on purpose
    cc $CFLAGS_BENCH -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$WORK/src_$sha/include" -I"$WORK/src_$sha/src" -o "$WORK/bin/bench_$sha" \
        "$WORK/src_tip/examples/perf_hil/experiments/large_sample_bench.c" "$WORK/src_$sha/src/tickle.c" \
        "$WORK/src_$sha/src/encoding.c" "$WORK/src_$sha/src/log.c" "$WORK/src_$sha/src/hal_linux.c" -lpthread -lm \
        2>"$WORK/build_$sha.log" || { cat "$WORK/build_$sha.log"; return 1; }
}

declare -A arm_bin arm_env arm_sha
read -r -a arm_specs <<<"$ARMS"
labels=()
for spec in "${arm_specs[@]}"; do
    IFS=: read -r label commit env <<<"$spec"
    sha=$(git -C "$TREE" rev-parse --short=8 "$commit") || exit 1
    build_bench "$sha" || exit 1
    arm_bin[$label]=$WORK/bin/bench_$sha
    arm_env[$label]=$(tr ',' ' ' <<<"${env:-}")
    arm_sha[$label]=$sha
    labels+=("$label")
done
build_bench "$tip_sha" || exit 1
pub_bin=$WORK/bin/bench_$tip_sha
base=${labels[0]}
arm_bin[BASE2]=${arm_bin[$base]}
arm_env[BASE2]=${arm_env[$base]}
arm_sha[BASE2]=${arm_sha[$base]}
labels+=(BASE2)
arms_line=""
for l in "${labels[@]}"; do arms_line+=" $l=${arm_sha[$l]}${arm_env[$l]:+[${arm_env[$l]}]}"; done

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
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 40)) $wrap env $envs $bin sub \
        -s $size -d $((DURATION + 8)) $qflag $SUB_FLAGS -b 192.168.88.255" >"$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 30)) $pub_bin pub \
        -s $size -r $rate -d $DURATION $qflag $PUB_FLAGS -b 192.168.88.255" >"$publog" 2>&1
    wait "$sub_pid"
    local p s iu ik cu ck tc cs rx st
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "RUN cell=$cell arm=$arm block=$block mode=$mode VOID no RESULT (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" | tee -a "$OUT"
        return
    fi
    # The subscriber's treatment, from its rx line: the ring's counters, the in-place reads, the empty reads.
    rx=$(grep -m1 -o 'rx: .*' "$sublog" | tr ' ' '\n' |
        grep -E '^(uring_[a-z]+|direct_reads|direct_runs|empty_reads|idle_skips|gro_enables|timer_[a-z]+|drain_starts)=' |
        tr '\n' ' ')
    if [ "$mode" = strace ]; then
        st=$(awk '$NF ~ /^[a-z_0-9]+$/ && $NF != "total" && NF >= 5 && $4 ~ /^[0-9]+$/ {
                  if (NF == 6) printf "sc_%s=%s/%s ", $NF, $4, $5; else printf "sc_%s=%s/0 ", $NF, $4 }' "$stlog")
        echo "RUN cell=$cell arm=$arm block=$block mode=strace $st$rx| $p | $s" >>"$OUT"
        echo "RUN cell=$cell arm=$arm block=$block mode=strace $(grep -oE 'sc_(io_uring_enter|epoll_pwait2|recvfrom|recvmsg)=[0-9/]*' <<<"$st" | tr '\n' ' ')"
        return
    fi
    iu=$(grep -m1 'instructions:u' "$perflog" 2>/dev/null | cut -d, -f1)
    ik=$(grep -m1 'instructions:k' "$perflog" 2>/dev/null | cut -d, -f1)
    cu=$(grep -m1 'cycles:u' "$perflog" 2>/dev/null | cut -d, -f1)
    ck=$(grep -m1 'cycles:k' "$perflog" 2>/dev/null | cut -d, -f1)
    tc=$(grep -m1 'task-clock' "$perflog" 2>/dev/null | cut -d, -f1)
    cs=$(grep -m1 'context-switches' "$perflog" 2>/dev/null | cut -d, -f1)
    echo "RUN cell=$cell arm=$arm block=$block mode=perf insn_u=${iu:-none} insn_k=${ik:-none} cyc_u=${cu:-none} cyc_k=${ck:-none} task_ms=${tc:-none} ctx=${cs:-none} $rx| $p | $s" >>"$OUT"
    echo "RUN cell=$cell arm=$arm block=$block insn_u=${iu:-none} insn_k=${ik:-none} $(grep -o 'delivered=[0-9]*' <<<"$s") $rx"
}

if [[ " $PARTS " == *" large "* ]]; then
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add rxua netns "$NS1" type veth peer name rxub netns "$NS2" || exit 1
    sudo -n ip -n "$NS1" addr add 192.168.88.1/24 dev rxua
    sudo -n ip -n "$NS2" addr add 192.168.88.2/24 dev rxub
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set rxua up
    sudo -n ip -n "$NS2" link set rxub up
    sudo -n ip netns exec "$NS1" tc qdisc add dev rxua root tbf rate 1gbit burst 64kb latency 100ms || exit 1
    echo "=== rx_uring_pc $(date -Is) tip=$tip_sha arms:$arms_line pub=$tip_sha pub_flags='$PUB_FLAGS' sub_flags='$SUB_FLAGS' blocks=$BLOCKS duration=${DURATION}s cflags='$CFLAGS_BENCH' load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"
    n=${#labels[@]}
    for block in $(seq 1 "$BLOCKS"); do # arms rotate per block (each block starts one arm later), cells in turn
        for spec in $CELLS; do
            IFS=: read -r cell size rate qflag <<<"$spec"
            for i in $(seq 0 $((n - 1))); do
                run "$cell" "${labels[$(((i + block - 1) % n))]}" "$block" "$size" "$rate" "$qflag" perf
            done
        done
    done
    for spec in $CELLS; do
        IFS=: read -r cell size rate qflag <<<"$spec"
        for l in "${labels[@]}"; do
            [ "$l" = BASE2 ] || run "$cell" "$l" s "$size" "$rate" "$qflag" strace
        done
    done
    echo "=== done $(date -Is) ===" | tee -a "$OUT"
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
fi
if [[ " $PARTS " == *" ping "* ]]; then
    # lat_gap_pc.sh as it is: its own namespaces, its own A/A (BASE2 of the first arm).
    ARMS="$ARMS" OUT="$OUT.ping" WORK="$WORK/ping" "$HERE/lat_gap_pc.sh" "$PING_BLOCKS" "$PING_SECS" | tail -3
fi
python3 "$HERE/rx_uring_pc_read.py" "$OUT" | tee -a "$OUT.read"
rc=${PIPESTATUS[0]}
