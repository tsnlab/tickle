#!/usr/bin/env bash
# initoptions8_segment_insn_pc.sh - user instructions per sample of the same-host segment path on THIS PC, in the very
# cells the rig's same-host gate v2 A/B of tt_InitOptions bundle 8 (segment options) ran. 2026-10-22.
#
# Why this exists: that rig A/B (ab_samehost_initoptions8_segment_20261022-003748, A=fab14fd9, B=94683aed) came out
# WORSE on the segment path - server user insn/sample +0.6% to +1.66% (BE p1/p3, reliable_throughput p3, latency
# p1/p3), client +0.6% on the latency cells - while the PC measurement made before it
# (initoptions8_segment_insn_pc_20261021-210540, run with heldslot_samehost_insn_pc.sh) said PASS. That run could not
# have seen it: it summed user AND kernel instructions, whose kernel half moves by 10-40% run to run on a shared PC, so
# its placement floors were 1-20% of the metric and every 1% user-space cost sat inside them; its cells were not the
# rig's (no reliable_throughput p3; 3 s windows with 1 s edges; -W/-C 1024 on the latency cells); and with 2 runs per
# arm a 0.6% shift was never going to clear its t threshold. This harness measures what the rig decides on:
# instructions:u alone, per the process's own samples, in the rig's cells.
#
# Arms (each built from a source tree with the bench's own build.sh in its own HOME, so no install prefix is shared):
#   A  the parent, TICKLE_LAYOUT_CFLAGS (gate_v2_lib.sh A1: -falign-functions=64, no loop/jump/label padding)
#   P  the parent, TICKLE_PLACEMENT_CFLAGS (A3: every function moved by a multiple of 64 bytes, nothing else) - the
#      placement control: the same instructions laid out differently. The change provably cannot touch it.
#   B  the change, TICKLE_LAYOUT_CFLAGS
# Blocks A P B B P A, REPS repetitions per block and cell. Both processes of a run in ONE private network namespace with
# ONE private /dev/shm (a tmpfs mounted inside `ip netns exec`), BENCH_IFACE=lo and a dummy link carrying 192.168.10.255,
# client on CPU_C and server on CPU_S, each under `perf stat -e instructions:u,instructions:k`. Cells (the rig's, ab
# summary "cells:"; DUR is the measured window, the edges are the rig's s6_transport_cells.sh WINDOW_ARGS):
#   be_p1 be_p3 be_p4  best_effort_throughput pN -K 1 -d DUR --warmup-s 2 --cooldown-s 2   (client and server)
#   rt_p3              reliable_throughput p3 -d DUR --warmup-s 2 --cooldown-s 2
#   lat_p1 lat_p3      reliable_latency pN -i 0.001 -d DUR_L -W 4096 -C 4096 -I 0.001     (server -d DUR_L+40)
#
# The metric is the gate's (ab_samehost.py COUNTS): a process's whole-life instructions:u over its own samples - the
# client's sent=, the throughput server's recv=, and for the latency server the client's sent= (round trips).
#
# PRE-REGISTERED READING (the python at the end; nothing else decides):
#   VOID   a run without both instructions:u counts or without the client's RESULT line, a client whose tx_shm= is 0 (the
#          run did not take the segment path, which is what is being measured), a throughput server without recv= > 0.
#          A void run is listed and excluded; an arm with fewer than 2 runs in a cell gives that metric NO VERDICT.
#   floor  |mean(P) - mean(A)| of the same cell and role: what moving every function alone does to the count.
#   PASS   B - A <= floor, or (B - A) / A <= +0.05%. (Below either is the change costing nothing this PC can tell from
#          layout.)
#   FAIL   B - A above the floor AND above +0.05% of A - on ANY of the 12 metrics (6 cells x client, server).
#   VERDICT PASS only when all 12 metrics PASS and nothing is void or without a verdict; FAIL when any metric FAILs;
#          else VOID. The sample counts (n per arm, and the samples per run) print before the values.
#   Also printed, never deciding: B - A and P - A in percent, A's own min-max spread, and instructions:k per sample.
#
# Learned from its first runs (2026-10-22; the rule above unchanged): on this PC the throughput servers (be_p1, be_p3,
# rt_p3) count in two modes a run - a reader that keeps catching the writer mid-copy polls the claimed head, at 0.13-0.34
# segment_head_stalls a sample against 0.03-0.04, and counts ~1.6-2x the instructions. Which mode a run lands in moves
# with speed and placement, not with code cost: the placement arm alone moved +16% to +68% there. Such a FAIL is read
# from the runs' head stalls before anything else; the latency cells and every client are deterministic to ~0.1%.
#
# Usage: A=<commit|@dir> B=<commit|@dir> [REPS=3] [DUR=5] [DUR_L=5] [CELLS="be_p1 be_p3 be_p4 rt_p3 lat_p1 lat_p3"]
#        [CPU_C=12] [CPU_S=13] [RECORD=0] [OUT=prefix] initoptions8_segment_insn_pc.sh
#   RECORD=1 also runs each process under `perf record -e instructions:u` and writes a per-symbol report next to it
#   ($OUT.d/<run>.<role>.syms) - for locating a difference, not for the verdict (perf record's own work is then counted
#   too; read the reports, not the table, from such a run).
#   OUT defaults to ~/rig_results_safe/initoptions8_segment_insn_pc_<stamp> ($OUT.txt, $OUT.d/). Needs `sudo -n ip`.
#   Launch detached (setsid nohup); the summary always ends with "=== initoptions8_segment_insn_pc ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
A=${A:?A=<commit or @dir>}
B=${B:?B=<commit or @dir>}
REPS=${REPS:-3}
DUR=${DUR:-5}
DUR_L=${DUR_L:-5}
CELLS=${CELLS:-"be_p1 be_p3 be_p4 rt_p3 lat_p1 lat_p3"}
CPU_C=${CPU_C:-12}
CPU_S=${CPU_S:-13}
RECORD=${RECORD:-0}
OUT=${OUT:-$HOME/rig_results_safe/initoptions8_segment_insn_pc_$(date +%Y%m%d-%H%M%S)}
# shellcheck disable=SC2034 # read by gate_v2_lib.sh
GATE_BUILD_ARCH=$(uname -m) # the arms build here, not on a Pi
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091
. "$HERE/gate_v2_lib.sh"
T0=$(date +%s)
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
WORK=$(mktemp -d "$HOME/tmp_build/initoptions8_insn_pc.XXXXXX") || exit 1
# The compilers' temporaries too: /tmp has a per-user quota that other sessions fill.
mkdir -p "$WORK/tmp" && export TMPDIR="$WORK/tmp"
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
on_exit() {
    local rc=$? ns
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== initoptions8_segment_insn_pc ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
say "=== initoptions8_segment_insn_pc $(date -Is): A=$A B=$B REPS=$REPS DUR=$DUR DUR_L=$DUR_L CELLS='$CELLS' RECORD=$RECORD harness=$(git -C "$REPO" rev-parse --short HEAD) ==="
command -v perf >/dev/null || {
    say "FATAL: no perf"
    exit 1
}

bench_of() { case $1 in be_*) echo best_effort_throughput ;; rt_*) echo reliable_throughput ;; lat_*) echo reliable_latency ;; esac; }

build_arm() { # $1 label, $2 commit or @dir, $3 cflags
    local label=$1 src=$2 flags=$3 tree="$WORK/$1/src" log="$OUT.d/build_$1.log" c v s
    mkdir -p "$tree" "$WORK/$label/home"
    if [ "${src:0:1}" = @ ]; then
        (cd "${src:1}" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf -) | tar -x -C "$tree"
        echo "source: tree ${src:1} at $(git -C "${src:1}" rev-parse HEAD) + diff $(git -C "${src:1}" diff HEAD | sha256sum | cut -c1-12) flags=$flags" >"$log"
    else
        git -C "$REPO" archive "$src" | tar -x -C "$tree"
        echo "source: commit $(git -C "$REPO" rev-parse "$src") flags=$flags" >"$log"
    fi
    for c in $CELLS; do
        v=$(bench_of "$c")
        s=${c#*_}
        [ -x "$WORK/bin/$label/${v}_$s/client" ] && continue
        (cd "$tree/examples/perf_hil/tickle" && HOME="$WORK/$label/home" TICKLE_EXTRA_CFLAGS="$flags" nice -n 10 ./build.sh "$v" "$s") \
            >>"$log" 2>&1 || {
            echo "BUILD_FAILED $v $s" >>"$log"
            return 1
        }
        mkdir -p "$WORK/bin/$label/${v}_$s"
        cp "$tree/examples/perf_hil/tickle/${v}_$s/client" "$tree/examples/perf_hil/tickle/${v}_$s/server" "$WORK/bin/$label/${v}_$s/"
        echo "BUILT $label ${v}_$s client=$(sha256sum "$WORK/bin/$label/${v}_$s/client" | cut -c1-16)" >>"$OUT.d/built.txt"
    done
}
# Two builds at a time at most (a shared PC): A and B together, then P.
build_arm A "$A" "$TICKLE_LAYOUT_CFLAGS" &
pa=$!
build_arm B "$B" "$TICKLE_LAYOUT_CFLAGS" &
pb=$!
fail=0
for p in $pa $pb; do wait "$p" || fail=1; done
build_arm P "$A" "$TICKLE_PLACEMENT_CFLAGS" || fail=1
[ $fail = 0 ] || {
    say "FATAL: a build failed - $OUT.d/build_*.log"
    exit 1
}
sort "$OUT.d/built.txt" | sed 's/^/    /' | tee -a "$SUM"
# P and A must differ in layout and nothing else: one identical client would make the floor a zero by construction.
for c in $CELLS; do
    v=$(bench_of "$c")
    s=${c#*_}
    if cmp -s "$WORK/bin/A/${v}_$s/client" "$WORK/bin/P/${v}_$s/client"; then
        say "FATAL: A and P built one client for $c - the placement flags did not reach the build"
        exit 1
    fi
done
say "    built in $(($(date +%s) - T0)) s"

NSID=0
run_one() { # $1 block, $2 arm, $3 cell, $4 rep
    local blk=$1 arm=$2 cell=$3 rep=$4 f v args sargs pre_c pre_s
    f="$OUT.d/b${blk}_${arm}_${cell}_r${rep}"
    NSID=$((NSID + 1))
    local na="i8p$$-${NSID}"
    echo "$na" >>"$NS_LIST"
    v="$(bench_of "$cell")_${cell#*_}"
    case "$cell" in
    be_*)
        args="-K 1 -d $DUR --warmup-s 2 --cooldown-s 2"
        sargs="-K 1 -d $((DUR + 40)) --warmup-s 2 --cooldown-s 2"
        ;;
    rt_*)
        args="-d $DUR --warmup-s 2 --cooldown-s 2"
        sargs="-d $((DUR + 40)) --warmup-s 2 --cooldown-s 2"
        ;;
    lat_*)
        args="-i 0.001 -d $DUR_L -W 4096 -C 4096 -I 0.001"
        sargs="-d $((DUR_L + 40)) -W 4096 -C 4096 -I 0.001"
        ;;
    esac
    pre_c="perf stat -x, -o $f.client.perf -e instructions:u,instructions:k --"
    pre_s="perf stat -x, -o $f.server.perf -e instructions:u,instructions:k --"
    if [ "$RECORD" = 1 ]; then
        pre_c="$pre_c perf record -q -e instructions:u -c 10007 -o $f.client.data --"
        pre_s="$pre_s perf record -q -e instructions:u -c 10007 -o $f.server.data --"
    fi
    local d="$WORK/bin/$arm/$v"
    if ! { sudo -n ip netns add "$na" && sudo -n ip netns exec "$na" ip link set lo up &&
        sudo -n ip netns exec "$na" ip route add default dev lo && sudo -n ip netns exec "$na" ip link add bench0 type dummy &&
        sudo -n ip netns exec "$na" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0 &&
        sudo -n ip netns exec "$na" ip link set bench0 up; }; then
        echo "no netns" >"$f.note"
        return 0
    fi
    # The server is the launching shell's own child ($!), stopped by that pid and its children whose exe is the server.
    # shellcheck disable=SC2024 # the logs are this user's, written by this shell
    sudo -n ip netns exec "$na" sh -c "mount -t tmpfs -o size=256m i8shm /dev/shm || exit 9
cd $d
env BENCH_IFACE=lo taskset -c $CPU_S $pre_s ./server $sargs > $f.server.log 2>&1 &
sp=\$!
sleep 1
env BENCH_IFACE=lo taskset -c $CPU_C $pre_c ./client $args; echo client_rc=\$?
for c in \$sp \$(cat /proc/\$sp/task/*/children 2>/dev/null) \$(for g in \$(cat /proc/\$sp/task/*/children 2>/dev/null); do cat /proc/\$g/task/*/children 2>/dev/null; done); do [ \"\$(readlink /proc/\$c/exe)\" = $d/server ] && kill -INT \$c; done
wait \$sp
chown -R $(id -u):$(id -g) $OUT.d" >"$f.client.log" 2>&1
    sudo -n ip netns del "$na"
    if [ "$RECORD" = 1 ]; then
        local r
        for r in client server; do
            [ -f "$f.$r.data" ] && perf report -i "$f.$r.data" --stdio --no-children --sort symbol -q 2>/dev/null |
                head -60 >"$f.$r.syms"
            rm -f "$f.$r.data"
        done
    fi
    echo "$(date +%T) b$blk $arm $cell r$rep $(grep -m1 '^RESULT' "$f.client.log" | tr ' ' '\n' | grep -E '^(sent|tx_shm|rtt_p50_ms)=' | tr '\n' ' ')" >>"$SUM"
}
blk=0
for arm in A P B B P A; do
    blk=$((blk + 1))
    say "--- block $blk arm $arm $(date -Is) ---"
    for rep in $(seq 1 "$REPS"); do
        for cell in $CELLS; do
            run_one $blk $arm "$cell" "$rep"
        done
    done
done
say "--- runs done at $(($(date +%s) - T0)) s ---"

python3 - "$OUT.d" "$CELLS" <<'EOF' | tee -a "$SUM"
import glob, os, re, sys
d, cells = sys.argv[1], sys.argv[2].split()
PCT_MAX = 0.05  # percent: B - A at or below this passes whatever the floor

def result(path):
    try:
        for line in open(path, errors="replace"):
            if line.startswith("RESULT"):
                return dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
    except OSError:
        pass
    return None

def count(path, event):
    try:
        for line in open(path):
            p = line.strip().split(",")
            if len(p) > 2 and p[2] == event and p[0].isdigit():
                return int(p[0])
    except OSError:
        pass
    return None

vals, kern, nsamp, voids = {}, {}, {}, []
for cl in sorted(glob.glob(os.path.join(d, "b*_*_*_r*.client.log"))):
    base = cl[:-len(".client.log")]
    m = re.match(r"b(\d+)_([APB])_(\w+?_p\d)_r(\d+)$", os.path.basename(base))
    if not m:
        continue
    _, arm, cell, _ = m.groups()
    tag = os.path.basename(base)
    r = result(cl)
    cu, su = count(base + ".client.perf", "instructions:u"), count(base + ".server.perf", "instructions:u")
    ck, sk = count(base + ".client.perf", "instructions:k"), count(base + ".server.perf", "instructions:k")
    if r is None or cu is None or su is None:
        voids.append(f"{tag}: client RESULT={r is not None} client insn:u={cu is not None} server insn:u={su is not None}")
        continue
    if int(r.get("tx_shm", "0") or 0) == 0:
        voids.append(f"{tag}: tx_shm={r.get('tx_shm')} (not on the segment path)")
        continue
    sent = int(r.get("sent", "0") or 0)
    if cell.startswith("lat_"):
        srv_n = sent
    else:
        s = result(base + ".server.log")
        srv_n = int(s.get("recv", "0") or 0) if s else 0
    if sent == 0 or srv_n == 0:
        voids.append(f"{tag}: client sent={sent} server samples={srv_n}")
        continue
    for role, u, k, n in (("client", cu, ck, sent), ("server", su, sk, srv_n)):
        vals.setdefault((cell, role), {}).setdefault(arm, []).append(u / n)
        if k is not None:
            kern.setdefault((cell, role), {}).setdefault(arm, []).append(k / n)
        nsamp.setdefault((cell, role), {}).setdefault(arm, []).append(n)

def mean(x):
    return sum(x) / len(x)

print("SAMPLE COUNTS (runs per arm A/P/B; samples per run min-max over all arms):")
metrics = [(c, r) for c in cells for r in ("client", "server")]
for key in metrics:
    n = nsamp.get(key, {})
    allv = [v for a in "APB" for v in n.get(a, [])]
    rng = f"{min(allv)}-{max(allv)}" if allv else "-"
    print(f"  {key[0]:7s} {key[1]:6s} n={len(n.get('A', []))}/{len(n.get('P', []))}/{len(n.get('B', []))} samples/run {rng}")
for v in voids:
    print("VOID run:", v)
print(f"USER INSTRUCTIONS PER SAMPLE (PASS: B-A <= |P-A| or B-A <= +{PCT_MAX}% of A):")
fails, nov = [], []
for key in metrics:
    arms = vals.get(key, {})
    if any(len(arms.get(a, [])) < 2 for a in "APB"):
        nov.append(key)
        print(f"  {key[0]:7s} {key[1]:6s} NO VERDICT n={[len(arms.get(a, [])) for a in 'APB']}")
        continue
    a, p, b = arms["A"], arms["P"], arms["B"]
    ma, mp, mb = mean(a), mean(p), mean(b)
    floor = abs(mp - ma)
    delta = mb - ma
    ok = delta <= floor or 100 * delta / ma <= PCT_MAX
    if not ok:
        fails.append(key)
    spread = 100 * (max(a) - min(a)) / ma
    print(f"  {key[0]:7s} {key[1]:6s} A={ma:10.2f} P-A={100*(mp-ma)/ma:+7.3f}% B-A={100*delta/ma:+7.3f}% "
          f"({delta:+8.2f} insn, floor {floor:7.2f}) A spread {spread:5.3f}% n={len(a)}/{len(p)}/{len(b)} "
          f"{'PASS' if ok else 'FAIL'}")
print("kernel instructions per sample (printed, never deciding):")
for key in metrics:
    arms = kern.get(key, {})
    if any(len(arms.get(x, [])) < 1 for x in "APB"):
        continue
    ma, mp, mb = (mean(arms[x]) for x in "APB")
    print(f"  {key[0]:7s} {key[1]:6s} insn:k A={ma:10.2f} P-A={100*(mp-ma)/ma:+7.2f}% B-A={100*(mb-ma)/ma:+7.2f}%")
if fails:
    print("VERDICT FAIL:", ", ".join(f"{c}/{r}" for c, r in fails))
elif voids or nov:
    print(f"VERDICT VOID: {len(voids)} void run(s), {len(nov)} metric(s) without a verdict")
else:
    print("VERDICT PASS")
EOF
