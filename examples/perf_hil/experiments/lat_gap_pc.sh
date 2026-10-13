#!/usr/bin/env bash
# lat_gap_pc.sh - the core reliable_latency ping-pong (campaign cells c10/c11/c16/c17's TickLE client and server, p1),
# commit by commit, on the PC: two private network namespaces over a veth, 192.168.10.0/24 (the bench's broadcast).
# Each arm runs twice: the CLIENT under perf stat (instructions and cycles, user and kernel) and under strace -f -c (its
# system calls by name, and their errors), each divided by the round trips it sent. The server stops by itself (its -d
# safety cap, then a SIGINT from its own timeout); nothing is stopped by name.
#
# Why (2026-10-13): the ab/rx-gap2 rig gate v2 chain (481b9e3e vs 0c9f9a3b) read the latency cells' client
# insn_k_per_sample +22..+24%, insn_u +3..+5.5%, server insn_u +0.5..+2.2%. Kernel instructions per round trip point at
# system calls per wake; this names them, per commit. NOT a rig figure (x86, veth).
#
# HOW TO READ IT - written before the first run: per arm, the mean over BLOCKS of client kernel instructions per round
# trip (PRIMARY) and system calls per round trip by name. The step that moves PRIMARY by more than max(A/A, 2 SE) and
# the calls whose per-round-trip count moves with it name the cause; a step whose calls move and whose PRIMARY does not
# is not it. ARMS as tx_gap_pc.sh ("label:commit[:ENV=V,...]"); the first arm is run twice (BASE2) for the A/A floor.
# Usage: lat_gap_pc.sh [BLOCKS] [SECONDS]   ARMS=, OUT= (default ~/largemsg_pc/lat_gap/results.txt), TREE=. Run it
# detached. Every bench process runs under ulimit -v and a timeout. The last line is always "lat_gap_pc done rc=<n>".
set -u
BLOCKS=${1:-3}
SECS=${2:-5}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_pc/lat_gap}
OUT=${OUT:-$WORK/results.txt}
ARMS=${ARMS:?ARMS=\"label:commit[:ENV=V,...] ...\"}
VMEM_KB=${VMEM_KB:-2097152}
INTERVAL=${INTERVAL:-0.001}
NS1=lg-cli-$$
NS2=lg-srv-$$
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "lat_gap_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK" "$TMPDIR"

declare -A arm_dir arm_env arm_sha
labels=()
for spec in $ARMS; do
    IFS=: read -r label commit env <<<"$spec"
    sha=$(git -C "$TREE" rev-parse --short=8 "$commit") || exit 1
    d=$WORK/src_$sha
    if [ ! -x "$d/examples/perf_hil/tickle/reliable_latency/client" ]; then
        rm -rf "$d" && mkdir -p "$d/home"
        git -C "$TREE" archive "$sha" | tar -x -C "$d" || exit 1
        (cd "$d/examples/perf_hil/tickle" && HOME=$d/home ./build.sh reliable_latency) >"$WORK/build_$sha.log" 2>&1 ||
            { tail -5 "$WORK/build_$sha.log"; exit 1; }
    fi
    arm_dir[$label]=$d/examples/perf_hil/tickle/reliable_latency
    arm_env[$label]=$(tr ',' ' ' <<<"${env:-}")
    arm_sha[$label]=$sha
    labels+=("$label")
done
arm_dir[BASE2]=${arm_dir[${labels[0]}]}
arm_env[BASE2]=${arm_env[${labels[0]}]}
arm_sha[BASE2]=${arm_sha[${labels[0]}]}
labels+=(BASE2)

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add lga netns "$NS1" type veth peer name lgb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev lga
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev lgb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set lga up
sudo -n ip -n "$NS2" link set lgb up
line="=== lat_gap_pc $(date -Is) arms:"
for l in "${labels[@]}"; do line+=" $l=${arm_sha[$l]}${arm_env[$l]:+[${arm_env[$l]}]}"; done
echo "$line blocks=$BLOCKS secs=$SECS interval=$INTERVAL load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

run() { # <arm> <block> <mode: perf|strace>
    local arm=$1 block=$2 mode=$3 dir=${arm_dir[$1]} envs=${arm_env[$1]}
    local slog=$WORK/srv.log clog=$WORK/cli.log meas=$WORK/meas.txt wrap
    rm -f "$meas"
    if [ "$mode" = strace ]; then wrap="strace -f -c -o $meas"
    else wrap="perf stat -x, -e instructions:u,instructions:k,cycles:u,cycles:k -o $meas"; fi
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; cd $dir && exec timeout -s INT $((SECS + 9)) env $envs \
        ./server -d $((SECS + 7)) -W 100 -C 100 -I 0.001" >"$slog" 2>&1 &
    local spid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; cd $dir && exec timeout -s INT $((SECS + 20)) $wrap env \
        $envs ./client -i $INTERVAL -d $SECS -W 100 -C 100 -I 0.001 -T 3" >"$clog" 2>&1
    wait "$spid"
    local res sent
    res=$(grep -m1 '^RESULT' "$clog")
    sent=$(grep -o ' sent=[0-9]*' <<<"$res" | cut -d= -f2)
    if [ -z "$sent" ] || [ "$sent" = 0 ]; then
        echo "RUN arm=$arm block=$block mode=$mode VOID (client: $(tail -1 "$clog"))" | tee -a "$OUT"
        return
    fi
    if [ "$mode" = strace ]; then
        local sc
        sc=$(awk -v n="$sent" '$NF != "total" && $NF ~ /^[a-z_0-9]+$/ && NF >= 5 {
                calls = $4; err = (NF == 6) ? $5 : 0; if (calls + 0 > 0) printf "%s=%.3f/%.3f ", $NF, calls / n, err / n }' "$meas")
        echo "RUN arm=$arm block=$block mode=strace sent=$sent per_rtt: $sc" >>"$OUT"
        echo "RUN arm=$arm block=$block mode=strace sent=$sent"
    else
        local iu ik cu ck
        iu=$(grep -m1 'instructions:u' "$meas" | cut -d, -f1); ik=$(grep -m1 'instructions:k' "$meas" | cut -d, -f1)
        cu=$(grep -m1 'cycles:u' "$meas" | cut -d, -f1); ck=$(grep -m1 'cycles:k' "$meas" | cut -d, -f1)
        echo "RUN arm=$arm block=$block mode=perf sent=$sent insn_u=$iu insn_k=$ik cyc_u=$cu cyc_k=$ck | $res" >>"$OUT"
        echo "RUN arm=$arm block=$block mode=perf sent=$sent insn_k/rtt=$((ik / sent)) insn_u/rtt=$((iu / sent))"
    fi
}

n=${#labels[@]}
for block in $(seq 1 "$BLOCKS"); do
    for i in $(seq 0 $((n - 1))); do
        run "${labels[$(((i + block - 1) % n))]}" "$block" perf
    done
done
for l in "${labels[@]}"; do [ "$l" = BASE2 ] || run "$l" s strace; done
echo "=== done $(date -Is) ===" | tee -a "$OUT"
rc=0
