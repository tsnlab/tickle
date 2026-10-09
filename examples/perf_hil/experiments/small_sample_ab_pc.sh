#!/usr/bin/env bash
# small_sample_ab_pc.sh - large-message stage 2's L2 on this PC (docs/DESIGN.md section 8): samples at or below 64 KB
# must not get worse. The parent commit (A) against this tree (B), large_sample_bench.c at both ends built against each
# with rmw's datagram and the shared-memory transport compiled out, between two private namespaces: 64 B (one
# datagram), 1,472 B (two fragments) and 64,000 B (44 fragments), BEST_EFFORT and RELIABLE KEEP_LAST 10, 1 kHz.
# Blocks A B B A, REPS runs per cell per block.
#
# HOW TO READ IT, written before the first run (2026-10-09): per cell and metric (subscriber latency median and p99,
# publisher and subscriber CPU per MB), B against A is HELD when |mean(B) - mean(A)| is within the A-against-A spread
# (|block 1 - block 4| means, the parent measured twice in the same session), WORSE or BETTER beyond it. Every run must
# deliver every sample with torn = 0, or it is VOID. A WORSE row is what L2 says blocks landing - on the rig, where
# ~/rig_queue_largemsg_A.sh repeats this with the vendor arms as controls; a PC row is an early warning only.
#
# Usage: small_sample_ab_pc.sh [PARENT] [REPS] [DURATION_S]   results to $OUT (default ~/largemsg_pc/small_ab.txt)
set -u
PARENT=${1:-a47b85e1}
REPS=${2:-2}
DURATION=${3:-10}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_pc}
OUT=${OUT:-$WORK/small_ab.txt}
NS1=lgab-pub-$$
NS2=lgab-sub-$$
PARENT_TREE=$WORK/parent_tree
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    git -C "$TREE" worktree remove --force "$PARENT_TREE" > /dev/null 2>&1
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK"
git -C "$TREE" worktree remove --force "$PARENT_TREE" > /dev/null 2>&1
git -C "$TREE" worktree add --detach "$PARENT_TREE" "$PARENT" > /dev/null 2>&1 || { echo "cannot check out $PARENT"; exit 1; }
build() { # tree out
    cc -O2 -DNDEBUG -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_SEGMENT_ENABLED=0 -I"$1/include" -I"$1/src" -o "$2" \
        "$HERE/large_sample_bench.c" "$1/src/tickle.c" "$1/src/encoding.c" "$1/src/log.c" "$1/src/hal_linux.c" \
        -lpthread -lm 2> "$2.build.log" || { cat "$2.build.log"; exit 1; }
}
build "$PARENT_TREE" "$WORK/bench_A"
build "$TREE" "$WORK/bench_B"
cmp -s "$WORK/bench_A" "$WORK/bench_B" && { echo "VOID: the two arms built the same binary"; exit 1; }

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add lgab1 netns "$NS1" type veth peer name lgab2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev lgab1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev lgab2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set lgab1 up
sudo -n ip -n "$NS2" link set lgab2 up

echo "=== small_sample_ab_pc $(date -Is) A=$PARENT B=$(git -C "$TREE" rev-parse --short HEAD)+worktree reps=$REPS ===" | tee -a "$OUT"
block=0
for arm in A B B A; do
    block=$((block + 1))
    for size in 64 1472 64000; do
        for mode in BE RELIABLE; do
            flags=""
            [ "$mode" = RELIABLE ] && flags="-R"
            for rep in $(seq 1 "$REPS"); do
                # shellcheck disable=SC2024,SC2086
                sudo -n ip netns exec "$NS2" "$WORK/bench_$arm" sub -s "$size" -d $((DURATION + 8)) $flags > "$WORK/ab_sub.log" 2>&1 &
                sub_pid=$!
                sleep 1
                # shellcheck disable=SC2024,SC2086
                sudo -n ip netns exec "$NS1" "$WORK/bench_$arm" pub -s "$size" -r 1000 -d "$DURATION" $flags > "$WORK/ab_pub.log" 2>&1
                wait "$sub_pid"
                p=$(grep -m1 '^RESULT:' "$WORK/ab_pub.log" | sed 's/^RESULT: //')
                s=$(grep -m1 '^RESULT:' "$WORK/ab_sub.log" | sed 's/^RESULT: //')
                echo "block=$block arm=$arm size=$size mode=$mode rep=$rep | $p | $s" | tee -a "$OUT"
            done
        done
    done
done
python3 - "$OUT" <<'EOF'
import re, sys, statistics
rows = {}
start = None
lines = open(sys.argv[1]).read().split("\n")
for i, line in enumerate(lines):
    if line.startswith("=== small_sample_ab_pc"):
        start = i
for line in lines[start:]:
    m = re.match(r"block=(\d) arm=(\w) size=(\d+) mode=(\w+) rep=\d \| (.*) \| (.*)", line)
    if not m:
        continue
    block, arm, size, mode, pub, sub = m.groups()
    g = lambda text, key: float(re.search(r"\b" + key + r"=([0-9.]+)", text).group(1))
    if g(sub, "delivered") != g(pub, "published") or g(sub, "torn") != 0:
        print(f"VOID run: block {block} {size} {mode}")
        continue
    for name, value in (("lat_med_us", g(sub, "latency_median_us")), ("lat_p99_us", g(sub, "latency_p99_us")),
                        ("pub_cpu_ms_per_mb", g(pub, "cpu_ms_per_mb")), ("sub_cpu_ms_per_mb", g(sub, "cpu_ms_per_mb"))):
        rows.setdefault((size, mode, name), {}).setdefault(int(block), []).append(value)
print("cell                         metric              A(b1,b4)          B(b2,b3)          B-A    A-A spread  verdict")
for (size, mode, name), blocks in sorted(rows.items(), key=lambda kv: (int(kv[0][0]), kv[0][1], kv[0][2])):
    if not all(b in blocks for b in (1, 2, 3, 4)):
        print(f"{size:>6} {mode:<9} {name:<18} incomplete")
        continue
    mean = lambda b: statistics.mean(blocks[b])
    a = statistics.mean(blocks[1] + blocks[4])
    b = statistics.mean(blocks[2] + blocks[3])
    spread = abs(mean(1) - mean(4))
    diff = b - a
    verdict = "HELD" if abs(diff) <= spread else ("WORSE" if diff > 0 else "BETTER")
    print(f"{size:>6} B {mode:<9}         {name:<18} {mean(1):8.2f},{mean(4):8.2f} {mean(2):8.2f},{mean(3):8.2f} "
          f"{diff:+8.2f} {spread:8.2f}    {verdict}")
EOF
