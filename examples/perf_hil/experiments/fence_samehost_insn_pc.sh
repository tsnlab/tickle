#!/usr/bin/env bash
# fence_samehost_insn_pc.sh - does the UDP-to-segment fence (tt_SEGMENT_SPAN_FENCE) cost the same-host steady state
# anything? Executed instructions per sample, PC only, gate v2's shape (TESTING.md 6.1) on this machine: 2026-10-11.
#
# Arms, built from source trees with the bench's own build.sh, each in its own HOME so no install prefix is shared:
#   A  the parent, -falign-functions=64 (layout-stable, gate v2 A1)
#   P  the parent, -falign-functions=128: the same instructions, every function moved - the placement floor (A3)
#   B  the change, -falign-functions=64
# Blocks A P B B P A, REPS repetitions per block and cell. Cells, both processes in ONE private namespace with ONE
# private /dev/shm (a tmpfs mounted inside `ip netns exec`), BENCH_IFACE=lo and a dummy link carrying 192.168.10.255,
# client on CPU_C and server on CPU_S, every process under `perf stat -e instructions:u,instructions:k`:
#   be_pN   best_effort_throughput pN -K 1 -d $DUR --warmup-s 1 --cooldown-s 1   (per sample: client / sent, server / recv)
#   lat_pN  reliable_latency pN -i 0.001 -d $DUR_L -W 1024 -C 1024 -I 0.001       (per round trip: both / client sent)
#
# PRE-REGISTERED READING (the python at the end; nothing else decides):
#   metric: instructions (user + kernel) per sample, per cell and role - 16 metrics.
#   VOID    any run without both perf counts or without a RESULT line, or a client whose tx_shm is 0 (the run did not
#           use the segment, so it says nothing about the segment's steady state), or fewer than 2 runs in an arm.
#   WORSE   B - A > 0 beyond the two-sided Welch t threshold at alpha 0.05 Bonferroni-corrected for 16 metrics, AND
#           B - A > |P - A| for the same metric (the floor measured in this run).
#   PASS    no metric WORSE. The pre-registered expectation is PASS with |B - A| / A < 0.5% on every be_pN metric:
#           the fence adds no instruction to a datagram's path (DESIGN.md section 10), so a WORSE here falsifies that.
#   Also printed, never deciding: B - A and P - A in percent, and the fence counters of arm B (shm_fences_*), which
#   should be 0 sent at startup here (a first attach is not fenced).
#
# Usage: A=<commit|@dir> B=<commit|@dir> [REPS=2] [DUR=3] [DUR_L=3] [CPU_C=12] [CPU_S=13] [OUT=prefix] \
#        fence_samehost_insn_pc.sh
#   OUT defaults to ~/rig_results_safe/fence_samehost_insn_pc_<stamp> ($OUT.txt, $OUT.d/). Needs `sudo -n ip`.
#   Launch detached; the summary always ends with "=== fence_samehost_insn_pc ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
A=${A:?A=<commit or @dir>}
B=${B:?B=<commit or @dir>}
REPS=${REPS:-2}
DUR=${DUR:-3}
DUR_L=${DUR_L:-3}
CPU_C=${CPU_C:-12}
CPU_S=${CPU_S:-13}
SIZES=${SIZES:-"p1 p2 p3 p4"}
OUT=${OUT:-$HOME/rig_results_safe/fence_samehost_insn_pc_$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
WORK=$(mktemp -d /tmp/fence_insn_pc.XXXXXX) || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
on_exit() {
    local rc=$? ns
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== fence_samehost_insn_pc ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
say "=== fence_samehost_insn_pc $(date -Is): A=$A B=$B REPS=$REPS DUR=$DUR DUR_L=$DUR_L sizes='$SIZES' harness=$(git -C "$REPO" rev-parse --short HEAD) ==="
command -v perf >/dev/null || { say "FATAL: no perf"; exit 1; }
EVENTS="instructions:u,instructions:k"

build_arm() { # $1 label, $2 commit or @dir, $3 extra cflags
    local label=$1 src=$2 flags=$3 tree="$WORK/$1/src" log="$OUT.d/build_$1.log" v s
    mkdir -p "$tree" "$WORK/$label/home"
    if [ "${src:0:1}" = @ ]; then
        (cd "${src:1}" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf -) | tar -x -C "$tree"
        echo "source: tree ${src:1} at $(git -C "${src:1}" rev-parse HEAD) + diff $(git -C "${src:1}" diff HEAD | sha256sum | cut -c1-12) flags=$flags" >"$log"
    else
        git -C "$REPO" archive "$src" | tar -x -C "$tree"
        echo "source: commit $(git -C "$REPO" rev-parse "$src") flags=$flags" >"$log"
    fi
    for s in $SIZES; do
        for v in best_effort_throughput reliable_latency; do
            (cd "$tree/examples/perf_hil/tickle" && HOME="$WORK/$label/home" TICKLE_EXTRA_CFLAGS="$flags" ./build.sh "$v" "$s") \
                >>"$log" 2>&1 || { echo "BUILD_FAILED $v $s" >>"$log"; return 1; }
            mkdir -p "$WORK/bin/$label/${v}_$s"
            cp "$tree/examples/perf_hil/tickle/${v}_$s/client" "$tree/examples/perf_hil/tickle/${v}_$s/server" \
                "$WORK/bin/$label/${v}_$s/"
            echo "BUILT $label ${v}_$s client=$(sha256sum "$WORK/bin/$label/${v}_$s/client" | cut -c1-16)" >>"$OUT.d/built.txt"
        done
    done
}
build_arm A "$A" "-falign-functions=64" &
pa=$!
build_arm P "$A" "-falign-functions=128" &
pp=$!
build_arm B "$B" "-falign-functions=64" &
pb=$!
fail=0
for p in $pa $pp $pb; do wait "$p" || fail=1; done
[ $fail = 0 ] || { say "FATAL: a build failed - $OUT.d/build_*.log"; exit 1; }
sort "$OUT.d/built.txt" | sed 's/^/    /' | tee -a "$SUM"
say "    built in $(($(date +%s) - T0)) s"

NSID=0
run_one() { # $1 block, $2 arm, $3 cell, $4 rep
    local blk=$1 arm=$2 cell=$3 rep=$4 f v args sargs
    f="$OUT.d/b${blk}_${arm}_${cell}_r${rep}"
    NSID=$((NSID + 1))
    local na="fip$$-${NSID}"
    echo "$na" >>"$NS_LIST"
    case "$cell" in
    be_*)
        v=best_effort_throughput_${cell#be_}
        args="-K 1 -d $DUR --warmup-s 1 --cooldown-s 1"
        sargs="$args"
        ;;
    lat_*)
        v=reliable_latency_${cell#lat_}
        args="-i 0.001 -d $DUR_L -W 1024 -C 1024 -I 0.001"
        sargs="-d $((DUR_L + 40))"
        ;;
    esac
    local d="$WORK/bin/$arm/$v"
    if ! { sudo -n ip netns add "$na" && sudo -n ip netns exec "$na" ip link set lo up &&
        sudo -n ip netns exec "$na" ip route add default dev lo && sudo -n ip netns exec "$na" ip link add bench0 type dummy &&
        sudo -n ip netns exec "$na" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0 &&
        sudo -n ip netns exec "$na" ip link set bench0 up; }; then
        echo "no netns" >"$f.note"
        return 0
    fi
    # The server is the launching shell's own child ($!), stopped by that pid (its exe checked) once the client is done.
    # shellcheck disable=SC2024 # the logs are this user's, written by this shell
    sudo -n ip netns exec "$na" sh -c "mount -t tmpfs -o size=256m fipshm /dev/shm || exit 9
cd $d
env BENCH_IFACE=lo taskset -c $CPU_S perf stat -x, -o $f.server.perf -e $EVENTS -- ./server $sargs > $f.server.log 2>&1 &
sp=\$!
sleep 1
env BENCH_IFACE=lo taskset -c $CPU_C perf stat -x, -o $f.client.perf -e $EVENTS -- ./client $args; echo client_rc=\$?
for c in \$sp \$(cat /proc/\$sp/task/*/children 2>/dev/null); do [ \"\$(readlink /proc/\$c/exe)\" = $d/server ] && kill -INT \$c; done
wait \$sp
chown -R $(id -u):$(id -g) $OUT.d" >"$f.client.log" 2>&1
    sudo -n ip netns del "$na"
    echo "$(date +%T) b$blk $arm $cell r$rep $(grep -m1 '^RESULT' "$f.client.log" | tr ' ' '\n' | grep -E '^(sent|tx_shm|rtt_p50_ms)=' | tr '\n' ' ')" >>"$SUM"
}
CELLS=()
for s in $SIZES; do CELLS+=("be_$s"); done
for s in $SIZES; do CELLS+=("lat_$s"); done
blk=0
for arm in A P B B P A; do
    blk=$((blk + 1))
    say "--- block $blk arm $arm $(date -Is) ---"
    for rep in $(seq 1 "$REPS"); do
        for cell in "${CELLS[@]}"; do
            run_one $blk $arm "$cell" "$rep"
        done
    done
done
say "--- runs done at $(($(date +%s) - T0)) s ---"

python3 - "$OUT.d" <<'EOF' | tee -a "$SUM"
import glob, math, os, re, sys
d = sys.argv[1]

def fields(path):
    try:
        for line in open(path, errors="replace"):
            if line.startswith("RESULT"):
                return dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
    except OSError:
        pass
    return None

def insn(path):
    total, seen = 0, 0
    try:
        for line in open(path):
            parts = line.strip().split(",")
            if len(parts) > 2 and parts[2].startswith("instructions") and parts[0].isdigit():
                total += int(parts[0]); seen += 1
    except OSError:
        return None
    return total if seen == 2 else None

def tcdf(t, df):  # Student t CDF via the regularized incomplete beta function
    x = df / (df + t * t)
    def betacf(a, b, x):
        qab, qap, qam, c, dd = a + b, a + 1, a - 1, 1.0, 1 - (a + b) * x / (a + 1)
        dd = 1 / (dd if abs(dd) > 1e-30 else 1e-30); h = dd
        for m in range(1, 300):
            m2 = 2 * m
            for aa in (m * (b - m) * x / ((qam + m2) * (a + m2)), -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))):
                dd = 1 + aa * dd; dd = 1 / (dd if abs(dd) > 1e-30 else 1e-30)
                c = 1 + aa / c; c = c if abs(c) > 1e-30 else 1e-30
                h *= dd * c
        return h
    a, b = df / 2, 0.5
    bt = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1 - x))
    ib = bt * betacf(a, b, x) / a if x < (a + 1) / (a + b + 2) else 1 - bt * betacf(b, a, 1 - x) / b
    return 1 - ib / 2 if t > 0 else ib / 2

def tquant(p, df):
    lo, hi = 0.0, 100.0
    for _ in range(200):
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if tcdf(mid, df) < p else (lo, mid)
    return lo

vals, voids = {}, []
for cl in sorted(glob.glob(os.path.join(d, "b*_*_*_r*.client.log"))):
    base = cl[:-len(".client.log")]
    m = re.match(r"b(\d+)_([APB])_(\w+?_p\d)_r(\d+)", os.path.basename(base))
    blk, arm, cell, rep = m.groups()
    r = fields(cl)
    ci, si = insn(base + ".client.perf"), insn(base + ".server.perf")
    tag = os.path.basename(base)
    if r is None or ci is None or si is None:
        voids.append(f"{tag}: RESULT={r is not None} client_counts={ci is not None} server_counts={si is not None}")
        continue
    if int(r.get("tx_shm", "0")) == 0:
        voids.append(f"{tag}: tx_shm={r.get('tx_shm')} (not on the segment)")
        continue
    sent = int(r.get("sent", "0"))
    if cell.startswith("be_"):
        s = fields(base + ".server.log")
        recv = int(s.get("recv", "0")) if s else 0
        if sent == 0 or recv == 0:
            voids.append(f"{tag}: sent={sent} recv={recv}"); continue
        per = {"client": ci / sent, "server": si / recv}
    else:
        if sent == 0:
            voids.append(f"{tag}: sent=0"); continue
        per = {"client": ci / sent, "server": si / sent}
    for role, v in per.items():
        vals.setdefault((cell, role), {}).setdefault(arm, []).append(v)

metrics = sorted(vals)
alpha = 0.05 / max(1, len(metrics))
print(f"metrics: {len(metrics)}; per-metric two-sided alpha {alpha:.5f}")
for v in voids:
    print("VOID run:", v)
worse, nov = [], []
def mean(x): return sum(x) / len(x)
def var(x): m = mean(x); return sum((y - m) ** 2 for y in x) / (len(x) - 1)
for key in metrics:
    arms = vals[key]
    if any(len(arms.get(a, [])) < 2 for a in "APB"):
        nov.append(key); print(f"{key[0]:8s} {key[1]:6s} NO VERDICT: n={[len(arms.get(a, [])) for a in 'APB']}"); continue
    a, p, b = arms["A"], arms["P"], arms["B"]
    ma, mp, mb = mean(a), mean(p), mean(b)
    va, vb = var(a) / len(a), var(b) / len(b)
    se = math.sqrt(va + vb) or 1e-12
    df = (va + vb) ** 2 / ((va ** 2 / (len(a) - 1) if va else 0) + (vb ** 2 / (len(b) - 1) if vb else 0) or 1e-30)
    df = max(1.0, min(df, 1e6))
    t = (mb - ma) / se
    thr = tquant(1 - alpha / 2, df)
    floor = abs(mp - ma)
    is_worse = (mb - ma) > 0 and t > thr and (mb - ma) > floor
    verdict = "WORSE" if is_worse else ("IMPROVED" if (ma - mb) > 0 and -t > thr and (ma - mb) > floor else "PASS")
    if is_worse: worse.append(key)
    print(f"{key[0]:8s} {key[1]:6s} A={ma:10.1f} P-A={100*(mp-ma)/ma:+6.2f}% B-A={100*(mb-ma)/ma:+6.2f}% "
          f"t={t:+7.2f} thr={thr:5.2f} floor={floor:8.1f} n={len(a)}/{len(p)}/{len(b)} {verdict}")
fences = []
for f in glob.glob(os.path.join(d, "b*_B_*.client.log")) + glob.glob(os.path.join(d, "b*_B_*.server.log")):
    for line in open(f, errors="replace"):
        m = re.search(r"shm_fences_sent=(\d+) shm_fences_passed=(\d+) shm_fences_expired=(\d+)", line)
        if m: fences.append(tuple(map(int, m.groups())))
print("arm B fence counters (sent, passed, expired) summed over",
      len(fences), "context lines:", tuple(sum(x[i] for x in fences) for i in range(3)) if fences else "none seen")
if voids or nov:
    print(f"VERDICT VOID: {len(voids)} void run(s), {len(nov)} metric(s) without a verdict")
elif worse:
    print("VERDICT WORSE:", ", ".join(f"{c}/{r}" for c, r in worse))
else:
    print("VERDICT PASS")
EOF
