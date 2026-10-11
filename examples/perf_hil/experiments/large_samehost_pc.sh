#!/usr/bin/env bash
# large_samehost_pc.sh - large-message step B (docs/DESIGN.md section 8, "Step B"): PC evidence for the same-host path.
#
# Two processes on this PC in ONE private network namespace (lo, a default route on it, loopback broadcast), with the
# segment and the step B region in a private directory under /dev/shm (TT_SEGMENT_DIR), so they are same-host peers
# and nothing else on the machine can see their files. large_sample_bench.c at both ends, built from this tree with
# rmw's datagram (tt_MAX_BUFFER_LENGTH 65507), large samples and the segment compiled in. Two arms, ABBA per rep:
#   A  -G 0   no region: step A, every fragment through the reader's 512-slot ring
#   B  -G N   a region of N chunks: one descriptor per sample (step B)
#
# NOT a rig figure and never to be tabled as one: one PC, loaded by whatever else runs on it. It answers "does step B
# deliver, whole, in order, where step A did not" and gives the direction of the CPU and latency change. The rig's
# same-host L4 cells are what DESIGN.md's B2 reads.
#
# HOW TO READ IT, written before the first run (2026-10-11), and enforced by verdict() below:
#   VOID (the run says nothing): an arm that did not get its treatment - arm B's publisher sent no descriptor
#     (large_ref_sent=0), or arm A's sent one; or a run without both RESULT lines.
#   CONTROL: CTRL_1KB (1,024 B RELIABLE, below 64 KB) takes no step B code at all, so its two arms must both deliver
#     every sample, and B's sub CPU per sample must lie within A's 2 x SE spread; if not, the session is VOID: the PC's
#     noise is larger than anything the other cells can show.
#   CORRECTNESS, arm B, every cell: torn=0 and out_of_order=0; RELIABLE delivered = published; BEST_EFFORT delivered
#     >= 99.9% of published. Any miss is a FAIL of step B, whatever the CPU says.
#   READING, arm B against arm A per cell: delivered %, sub + pub CPU per MB, latency median - each IMPROVED / WORSE
#     beyond 2 x SE of the difference of means, else HELD. Step B's expected outcome: BE 4 MB delivered goes from ~0
#     to ~100%; sub + pub CPU per MB IMPROVED in every large cell. A large cell where CPU is not IMPROVED is the PC's
#     first sign of B2.3's falsifier (step B pays nothing) and is reported as such, not explained away.
#
# Usage: large_samehost_pc.sh [REPS] [DURATION_S]      results to $OUT (default ~/largemsg_pc/samehost_<time>.txt)
# Needs passwordless `sudo ip`. Long: launch it detached (CLAUDE.md, "Long-running work").
set -u
REPS=${1:-3}
DURATION=${2:-20}
CHUNKS=${CHUNKS:-13} # min(depth 10, tt_LARGE_RETAINED) + tt_LARGE_REGION_SPARE, as rmw_tickle sizes it
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_pc}
OUT=${OUT:-$WORK/samehost_$(date +%Y%m%d-%H%M%S).txt}
NS=lgsh-$$
# Short: the region names (tickle-large-<ip>-<port>-<id>-<entity>) must fit tt_SEGMENT_PATH_LENGTH with it.
SHMDIR=/dev/shm/lg$$
BIN=$WORK/large_samehost_bench_$$
BCAST=127.255.255.255

cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    rm -rf "$SHMDIR" "$BIN"
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK" "$SHMDIR" || exit 1
sha=$(git -C "$TREE" rev-parse --short HEAD)
dirty=$(git -C "$TREE" status --porcelain -- src include | wc -l)
cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 "-DTT_SEGMENT_DIR=\"$SHMDIR/\"" \
    -I"$TREE/include" -I"$TREE/src" -o "$BIN" "$HERE/large_sample_bench.c" "$TREE/src/tickle.c" \
    "$TREE/src/encoding.c" "$TREE/src/log.c" "$TREE/src/hal_linux.c" -lpthread -lm || exit 1

sudo -n ip netns add "$NS" || exit 1
sudo -n ip -n "$NS" link set lo up || exit 1
sudo -n ip -n "$NS" route add default dev lo || exit 1 # a lo-only namespace has no route (CLAUDE.md)

echo "=== large_samehost_pc $(date -Is) tree=$sha dirty_core_files=$dirty reps=$REPS duration=${DURATION}s" \
    "chunks=$CHUNKS shm=$SHMDIR ===" | tee -a "$OUT"

# run CELL ARM SIZE FLAGS REP: one pub/sub pair; prints the row and records it in $OUT.
run() {
    local cell=$1 arm=$2 size=$3 flags=$4 rep=$5 g=0
    [ "$arm" = B ] && g=$CHUNKS
    local sublog=$WORK/lgsh_sub_$$.log publog=$WORK/lgsh_pub_$$.log
    rm -f "$SHMDIR"/* 2>/dev/null
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS" "$BIN" sub -s "$size" -d $((DURATION + 8)) -b "$BCAST" $flags > "$sublog" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS" "$BIN" pub -s "$size" -r 30 -d "$DURATION" -b "$BCAST" -G "$g" $flags > "$publog" 2>&1
    wait "$sub_pid"
    local p s
    p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
    s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
    if [ -z "$p" ] || [ -z "$s" ]; then
        echo "ROW $cell $arm rep$rep VOID no RESULT line (pub: $(tail -1 "$publog") / sub: $(tail -1 "$sublog"))" |
            tee -a "$OUT"
        return
    fi
    echo "ROW $cell $arm rep$rep | $p | $s" | tee -a "$OUT"
}

# cell NAME SIZE FLAGS: REPS reps, the arms in ABBA order rotating per rep.
CELLS=()
cell() {
    local name=$1 size=$2 flags=$3 rep
    CELLS+=("$name")
    for rep in $(seq 1 "$REPS"); do
        if [ $((rep % 2)) = 1 ]; then
            run "$name" A "$size" "$flags" "$rep"
            run "$name" B "$size" "$flags" "$rep"
        else
            run "$name" B "$size" "$flags" "$rep"
            run "$name" A "$size" "$flags" "$rep"
        fi
    done
}

cell CTRL_1KB 1024 "-R"
cell BE_1MB 1048576 ""
cell BE_4MB 4194304 ""
cell KEEP_LAST_1MB 1048576 "-R"
cell KEEP_LAST_4MB 4194304 "-R"

# --- the reading rules, applied to the rows above
(python3 - "$OUT" <<'PY'
import math, re, sys
out = sys.argv[1]
rows = {}
void = []
for line in open(out):
    if not line.startswith("ROW "):
        continue
    parts = line.split()
    cell, arm, rep = parts[1], parts[2], parts[3]
    if "VOID" in parts[4:6]:
        void.append(f"{cell} {arm} {rep}: no RESULT")
        continue
    pub, sub = line.split(" | ")[1], line.split(" | ")[2]
    f = lambda text, key: float(re.search(r"\b" + key + r"=([0-9.]+)", text).group(1))
    r = dict(published=f(pub, "published"), delivered=f(sub, "delivered"), torn=f(sub, "torn"),
             ooo=f(sub, "out_of_order"), lat=f(sub, "latency_median_us"), sub_cpu=f(sub, "cpu_ms_per_mb"),
             pub_cpu=f(pub, "cpu_ms_per_mb"), refs=f(pub, "large_ref_sent"), reliable=f(sub, "reliable"),
             sub_rss=f(sub, "peak_rss_kb"), pub_rss=f(pub, "peak_rss_kb"), sub_ms=f(sub, "cpu_ms"))
    if (arm == "B") != (r["refs"] > 0) and cell != "CTRL_1KB":
        void.append(f"{cell} {arm} {rep}: treatment not applied (large_ref_sent={int(r['refs'])})")
        continue
    rows.setdefault(cell, {}).setdefault(arm, []).append(r)

def mean_se(xs):
    m = sum(xs) / len(xs)
    if len(xs) < 2:
        return m, float("inf")
    return m, math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1) / len(xs))

def compare(a, b, lower_is_better=True):
    (ma, sa), (mb, sb) = mean_se(a), mean_se(b)
    se = math.sqrt(sa * sa + sb * sb)
    d = mb - ma
    if abs(d) <= 2 * se:
        return "HELD", ma, mb
    better = d < 0 if lower_is_better else d > 0
    return ("IMPROVED" if better else "WORSE"), ma, mb

print("=== verdicts (PC indication, not a rig figure) ===")
for v in void:
    print("VOID run:", v)
ctrl = rows.get("CTRL_1KB", {})
session_void = False
if len(ctrl.get("A", [])) < 2 or len(ctrl.get("B", [])) < 2:
    print("CONTROL: VOID - fewer than 2 runs per arm"); session_void = True
else:
    full = all(r["delivered"] == r["published"] for arm in "AB" for r in ctrl[arm])
    per_sample = lambda rs: [r["sub_ms"] / max(r["delivered"], 1) for r in rs]
    v, ma, mb = compare(per_sample(ctrl["A"]), per_sample(ctrl["B"]))
    print(f"CONTROL CTRL_1KB: delivered all={full}; sub cpu ms/sample A {ma:.4f} B {mb:.4f} -> {v}")
    if not full or v != "HELD":
        print("CONTROL FAILED: the session is VOID"); session_void = True
fails = 0
for cell in [c for c in rows if c != "CTRL_1KB"]:
    a, b = rows[cell].get("A", []), rows[cell].get("B", [])
    if not a or not b:
        print(f"{cell}: VOID - an arm has no usable run"); continue
    for r in b:
        ok = r["torn"] == 0 and r["ooo"] == 0 and (
            r["delivered"] == r["published"] if r["reliable"] else r["delivered"] >= 0.999 * r["published"])
        if not ok:
            fails += 1
            print(f"{cell} B CORRECTNESS FAIL: delivered {int(r['delivered'])}/{int(r['published'])} "
                  f"torn {int(r['torn'])} out_of_order {int(r['ooo'])}")
    pct = lambda rs: [100.0 * r["delivered"] / max(r["published"], 1) for r in rs]
    cpu = lambda rs: [r["sub_cpu"] + r["pub_cpu"] for r in rs]
    lat = lambda rs: [r["lat"] for r in rs]
    dv, da, db = compare(pct(a), pct(b), lower_is_better=False)
    cv, ca, cb = compare(cpu(a), cpu(b))
    lv, la, lb = compare(lat(a), lat(b))
    rss = lambda rs, k: max(r[k] for r in rs)
    print(f"{cell}: n={len(a)}/{len(b)} delivered% A {da:.2f} B {db:.2f} -> {dv}; cpu ms/MB sub+pub A {ca:.3f} "
          f"B {cb:.3f} -> {cv}; latency median us A {la:.0f} B {lb:.0f} -> {lv}; peak rss kB sub "
          f"{rss(a,'sub_rss')}/{rss(b,'sub_rss')} pub {rss(a,'pub_rss')}/{rss(b,'pub_rss')}")
    if cv != "IMPROVED":
        print(f"{cell}: CPU not IMPROVED - the PC's first sign of B2.3's falsifier (step B pays nothing)")
print("SESSION:", "VOID" if session_void else ("FAIL" if fails else "PASS (correctness)"))
PY
) | tee -a "$OUT"
echo "=== done $(date -Is) ===" | tee -a "$OUT"
