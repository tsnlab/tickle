#!/usr/bin/env bash
# revalidate_insn_pc.sh - kernel instructions per sample of a low-rate same-host reliable latency pair on THIS PC, parent
# against a revalidation that keeps its mapping (branch ab/revalidate-no-remap). 2026-10-23.
#
# Why this exists: 5a made an attached peer segment revalidate every segment.revalidate_ns (1 s), and each revalidation
# re-attached - munmap of the 768 KiB ring, the bell FIFO closed and reopened, open/fstat/flock/mmap, the ring faulted in
# again. The rig's same-host gate v2 (ab_samehost_segment_time_limits_20261023-063521) read it as reliable_latency p3
# server kernel instructions +1.95% a sample (client shm_attach_attempts 5 -> 9 a run). The change asks the name with
# tt_segment_identify() (open, fstat, flock look, close) and keeps the mapping when the same file is there, owned. At
# 1,000 round trips a second that removes about one re-attach per 1,000 samples on each side.
#
# Arms (each built from a source tree with the bench's own build.sh in its own HOME, so no install prefix is shared):
#   A  the parent, TICKLE_LAYOUT_CFLAGS (gate_v2_lib.sh)
#   P  the parent, TICKLE_PLACEMENT_CFLAGS - the control: the same code laid out differently, so the change provably
#      cannot touch it. |P - A| is the floor a difference must clear, for kernel instructions as for user ones.
#   B  the change, TICKLE_LAYOUT_CFLAGS
# Blocks A P B B P A, REPS repetitions per block and cell. Both processes of a run in ONE private network namespace with
# ONE private /dev/shm (a tmpfs mounted inside `ip netns exec`), BENCH_IFACE=lo and a dummy link carrying 192.168.10.255,
# client on CPU_C and server on CPU_S, each under `perf stat -e instructions:u,instructions:k`, address space capped at
# 2 GB. Cells: lat_p1 lat_p3 = reliable_latency pN -i 0.001 -d DUR_L -W 4096 -C 4096 -I 0.001 (the rig's), server
# killed with SIGINT when the client ends. The metric is a process's whole-life count over the round trips (client
# sent=), as ab_samehost.py counts it.
#
# PRE-REGISTERED READING (the python at the end; nothing else decides):
#   VOID run   no client RESULT line, a missing instructions:u or :k count, tx_shm= 0 (not on the segment path) or
#              sent= 0. Listed and excluded; an arm with fewer than 2 runs in a cell gives that metric NO VERDICT.
#   TREATMENT  per cell, the client's shm_attach_attempts (every attach it made, any outcome): median(A) - median(B) must
#              be at least DUR_L/4 (A re-attaches about once a second, B never after its first attaches), and
#              |median(P) - median(A)| at most 2 (the same code). Otherwise the cell is VOID - the arms did not differ in
#              what is being changed, or the control did.
#   floor      |mean(P) - mean(A)| of the same cell, role and event.
#   kernel     per cell and role: IMPROVED when A - B > floor and (A - B) / A > 0.5%; WORSE when B - A > floor and
#              (B - A) / A > 0.5%; HELD otherwise.
#   user       per cell and role: FAIL when B - A > floor and > +0.05% of A (identify's user-space cost, one call a
#              second, must not show); PASS otherwise.
#   VERDICT    PASS when both servers' kernel metrics are IMPROVED, no kernel metric is WORSE, no user metric FAILs, and
#              no run, cell or metric is void or without a verdict. FAIL - the hypothesis falsified - when any kernel
#              metric is WORSE, any user metric FAILs, or a server's kernel metric is HELD. VOID otherwise.
#   Also printed, never deciding: B - A and P - A in percent, A's min-max spread, the load average at each run's start.
#
# Usage: A=<commit|@dir> B=<commit|@dir> [REPS=3] [DUR_L=20] [CELLS="lat_p1 lat_p3"] [CPU_C=12] [CPU_S=13]
#        [LOAD_MAX=4] [WAIT_QUIET_S=7200] [OUT=prefix] revalidate_insn_pc.sh
#   Waits (up to WAIT_QUIET_S) for a 1-minute load average below LOAD_MAX before the runs: the PC is shared.
#   OUT defaults to ~/rig_results_safe/revalidate_insn_pc_<stamp> ($OUT.txt, $OUT.d/). Needs `sudo -n ip`.
#   Launch detached (setsid nohup); the summary always ends with "=== revalidate_insn_pc ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
A=${A:?A=<commit or @dir>}
B=${B:?B=<commit or @dir>}
REPS=${REPS:-3}
DUR_L=${DUR_L:-20}
CELLS=${CELLS:-"lat_p1 lat_p3"}
CPU_C=${CPU_C:-12}
CPU_S=${CPU_S:-13}
LOAD_MAX=${LOAD_MAX:-4}
WAIT_QUIET_S=${WAIT_QUIET_S:-7200}
OUT=${OUT:-$HOME/rig_results_safe/revalidate_insn_pc_$(date +%Y%m%d-%H%M%S)}
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
mkdir -p "$HOME/tmp_build" || exit 1
WORK=$(mktemp -d "$HOME/tmp_build/revalidate_insn_pc.XXXXXX") || exit 1
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
    say "=== revalidate_insn_pc ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
say "=== revalidate_insn_pc $(date -Is): A=$A B=$B REPS=$REPS DUR_L=$DUR_L CELLS='$CELLS' LOAD_MAX=$LOAD_MAX harness=$(git -C "$REPO" rev-parse --short HEAD) ==="
command -v perf >/dev/null || {
    say "FATAL: no perf"
    exit 1
}

load1() { cut -d' ' -f1 /proc/loadavg; }

build_arm() { # $1 label, $2 commit or @dir, $3 cflags
    local label=$1 src=$2 flags=$3 tree="$WORK/$1/src" log="$OUT.d/build_$1.log" c s
    mkdir -p "$tree" "$WORK/$label/home"
    if [ "${src:0:1}" = @ ]; then
        (cd "${src:1}" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf -) | tar -x -C "$tree"
        echo "source: tree ${src:1} at $(git -C "${src:1}" rev-parse HEAD) + diff $(git -C "${src:1}" diff HEAD | sha256sum | cut -c1-12) flags=$flags" >"$log"
    else
        git -C "$REPO" archive "$src" | tar -x -C "$tree"
        echo "source: commit $(git -C "$REPO" rev-parse "$src") flags=$flags" >"$log"
    fi
    for c in $CELLS; do
        s=${c#*_}
        [ -x "$WORK/bin/$label/reliable_latency_$s/client" ] && continue
        (cd "$tree/examples/perf_hil/tickle" && HOME="$WORK/$label/home" TICKLE_EXTRA_CFLAGS="$flags" nice -n 10 ./build.sh reliable_latency "$s") \
            >>"$log" 2>&1 || {
            echo "BUILD_FAILED reliable_latency $s" >>"$log"
            return 1
        }
        mkdir -p "$WORK/bin/$label/reliable_latency_$s"
        cp "$tree/examples/perf_hil/tickle/reliable_latency_$s/client" "$tree/examples/perf_hil/tickle/reliable_latency_$s/server" \
            "$WORK/bin/$label/reliable_latency_$s/"
        echo "BUILT $label reliable_latency_$s client=$(sha256sum "$WORK/bin/$label/reliable_latency_$s/client" | cut -c1-16)" >>"$OUT.d/built.txt"
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
    if cmp -s "$WORK/bin/A/reliable_latency_${c#*_}/client" "$WORK/bin/P/reliable_latency_${c#*_}/client"; then
        say "FATAL: A and P built one client for $c - the placement flags did not reach the build"
        exit 1
    fi
done
say "    built in $(($(date +%s) - T0)) s"

waited=0
while awk -v l="$(load1)" -v m="$LOAD_MAX" 'BEGIN { exit !(l >= m) }'; do
    if [ "$waited" -ge "$WAIT_QUIET_S" ]; then
        say "FATAL: load $(load1) still >= $LOAD_MAX after ${WAIT_QUIET_S} s - the PC is not quiet"
        exit 1
    fi
    sleep 60
    waited=$((waited + 60))
done
say "    quiet after ${waited} s: load $(cat /proc/loadavg)"

NSID=0
run_one() { # $1 block, $2 arm, $3 cell, $4 rep
    local blk=$1 arm=$2 cell=$3 rep=$4 f v args sargs pre_c pre_s
    f="$OUT.d/b${blk}_${arm}_${cell}_r${rep}"
    NSID=$((NSID + 1))
    local na="rvp$$-${NSID}"
    echo "$na" >>"$NS_LIST"
    v="reliable_latency_${cell#*_}"
    args="-i 0.001 -d $DUR_L -W 4096 -C 4096 -I 0.001"
    sargs="-d $((DUR_L + 40)) -W 4096 -C 4096 -I 0.001"
    pre_c="perf stat -x, -o $f.client.perf -e instructions:u,instructions:k --"
    pre_s="perf stat -x, -o $f.server.perf -e instructions:u,instructions:k --"
    local d="$WORK/bin/$arm/$v"
    echo "load1=$(load1)" >"$f.load"
    if ! { sudo -n ip netns add "$na" && sudo -n ip netns exec "$na" ip link set lo up &&
        sudo -n ip netns exec "$na" ip route add default dev lo && sudo -n ip netns exec "$na" ip link add bench0 type dummy &&
        sudo -n ip netns exec "$na" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0 &&
        sudo -n ip netns exec "$na" ip link set bench0 up; }; then
        echo "no netns" >"$f.note"
        return 0
    fi
    # The server is the launching shell's own child ($!), stopped by that pid and its children whose exe is the server.
    # shellcheck disable=SC2024 # the logs are this user's, written by this shell
    sudo -n ip netns exec "$na" sh -c "mount -t tmpfs -o size=256m rvshm /dev/shm || exit 9
ulimit -v 2097152
cd $d
env BENCH_IFACE=lo taskset -c $CPU_S $pre_s ./server $sargs > $f.server.log 2>&1 &
sp=\$!
sleep 1
env BENCH_IFACE=lo taskset -c $CPU_C $pre_c ./client $args; echo client_rc=\$?
for c in \$sp \$(cat /proc/\$sp/task/*/children 2>/dev/null) \$(for g in \$(cat /proc/\$sp/task/*/children 2>/dev/null); do cat /proc/\$g/task/*/children 2>/dev/null; done); do [ \"\$(readlink /proc/\$c/exe)\" = $d/server ] && kill -INT \$c; done
wait \$sp
chown -R $(id -u):$(id -g) $OUT.d" >"$f.client.log" 2>&1
    sudo -n ip netns del "$na"
    echo "$(date +%T) b$blk $arm $cell r$rep $(cat "$f.load") $(grep -m1 '^RESULT' "$f.client.log" | tr ' ' '\n' | grep -E '^(sent|tx_shm|rtt_p50_ms|shm_attach_attempts)=' | tr '\n' ' ')" >>"$SUM"
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

python3 - "$OUT.d" "$CELLS" "$DUR_L" <<'EOF' | tee -a "$SUM"
import glob, os, re, statistics, sys
d, cells, dur = sys.argv[1], sys.argv[2].split(), float(sys.argv[3])
USER_PCT_MAX = 0.05  # percent: user B - A at or below this passes whatever the floor
KERN_PCT = 0.5       # percent: a kernel difference must clear this as well as the floor to count

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

vals = {"u": {}, "k": {}}
attaches, nsamp, voids = {}, {}, []
for cl in sorted(glob.glob(os.path.join(d, "b*_*_*_r*.client.log"))):
    base = cl[:-len(".client.log")]
    m = re.match(r"b(\d+)_([APB])_(\w+?_p\d)_r(\d+)$", os.path.basename(base))
    if not m:
        continue
    _, arm, cell, _ = m.groups()
    tag = os.path.basename(base)
    r = result(cl)
    c = {(role, ev): count(f"{base}.{role}.perf", f"instructions:{ev}") for role in ("client", "server") for ev in "uk"}
    if r is None or any(v is None for v in c.values()):
        voids.append(f"{tag}: client RESULT={r is not None} counts={ {f'{k[0]}:{k[1]}': v is not None for k, v in c.items()} }")
        continue
    if int(r.get("tx_shm", "0") or 0) == 0:
        voids.append(f"{tag}: tx_shm={r.get('tx_shm')} (not on the segment path)")
        continue
    sent = int(r.get("sent", "0") or 0)
    if sent == 0:
        voids.append(f"{tag}: client sent=0")
        continue
    if "shm_attach_attempts" not in r:
        voids.append(f"{tag}: no shm_attach_attempts (the treatment cannot be read)")
        continue
    attaches.setdefault(cell, {}).setdefault(arm, []).append(int(r["shm_attach_attempts"]))
    for role in ("client", "server"):
        for ev in "uk":
            vals[ev].setdefault((cell, role), {}).setdefault(arm, []).append(c[(role, ev)] / sent)
        nsamp.setdefault((cell, role), {}).setdefault(arm, []).append(sent)

def mean(x):
    return sum(x) / len(x)

print("SAMPLE COUNTS (runs per arm A/P/B; round trips per run min-max over all arms):")
metrics = [(c, r) for c in cells for r in ("client", "server")]
for key in metrics:
    n = nsamp.get(key, {})
    allv = [v for a in "APB" for v in n.get(a, [])]
    rng = f"{min(allv)}-{max(allv)}" if allv else "-"
    print(f"  {key[0]:7s} {key[1]:6s} n={len(n.get('A', []))}/{len(n.get('P', []))}/{len(n.get('B', []))} round trips/run {rng}")
for v in voids:
    print("VOID run:", v)

print(f"TREATMENT (client shm_attach_attempts a run; A - B must be >= {dur / 4:.1f}, |P - A| <= 2):")
void_cells = []
for cell in cells:
    a = attaches.get(cell, {})
    if any(len(a.get(x, [])) < 1 for x in "APB"):
        void_cells.append(cell)
        print(f"  {cell:7s} VOID: no runs in some arm {[len(a.get(x, [])) for x in 'APB']}")
        continue
    ma, mp, mb = (statistics.median(a[x]) for x in "APB")
    ok = ma - mb >= dur / 4 and abs(mp - ma) <= 2
    if not ok:
        void_cells.append(cell)
    print(f"  {cell:7s} median A={ma} P={mp} B={mb} (A {a['A']}, B {a['B']}) {'applied' if ok else 'VOID'}")

fails, held_servers, nov, improved_servers = [], [], [], []
for ev, title in (("k", "KERNEL"), ("u", "USER")):
    if ev == "k":
        print(f"{title} INSTRUCTIONS PER ROUND TRIP (IMPROVED: A-B > |P-A| and > {KERN_PCT}% of A; WORSE: the mirror):")
    else:
        print(f"{title} INSTRUCTIONS PER ROUND TRIP (FAIL: B-A > |P-A| and > +{USER_PCT_MAX}% of A):")
    for key in metrics:
        arms = vals[ev].get(key, {})
        if key[0] in void_cells or any(len(arms.get(a, [])) < 2 for a in "APB"):
            nov.append((ev, key))
            print(f"  {key[0]:7s} {key[1]:6s} NO VERDICT n={[len(arms.get(a, [])) for a in 'APB']}")
            continue
        a, p, b = arms["A"], arms["P"], arms["B"]
        ma, mp, mb = mean(a), mean(p), mean(b)
        floor = abs(mp - ma)
        delta = mb - ma
        pct = 100 * delta / ma
        if ev == "k":
            if -delta > floor and -pct > KERN_PCT:
                word = "IMPROVED"
                if key[1] == "server":
                    improved_servers.append(key)
            elif delta > floor and pct > KERN_PCT:
                word = "WORSE"
                fails.append((ev, key))
            else:
                word = "HELD"
                if key[1] == "server":
                    held_servers.append(key)
        else:
            word = "FAIL" if delta > floor and pct > USER_PCT_MAX else "PASS"
            if word == "FAIL":
                fails.append((ev, key))
        spread = 100 * (max(a) - min(a)) / ma
        print(f"  {key[0]:7s} {key[1]:6s} A={ma:10.2f} P-A={100*(mp-ma)/ma:+7.3f}% B-A={pct:+7.3f}% "
              f"({delta:+9.2f} insn, floor {floor:8.2f}) A spread {spread:5.3f}% n={len(a)}/{len(p)}/{len(b)} {word}")

loads = []
for lf in glob.glob(os.path.join(d, "*.load")):
    try:
        loads.append(float(open(lf).read().split("=")[1]))
    except (OSError, ValueError, IndexError):
        pass
if loads:
    print(f"load1 at run start (never deciding): min {min(loads):.2f} median {statistics.median(loads):.2f} max {max(loads):.2f}")
if fails or held_servers:
    print("VERDICT FAIL:", ", ".join([f"{ev}:{c}/{r} worse" for ev, (c, r) in fails] +
                                     [f"k:{c}/{r} held" for c, r in held_servers]))
elif voids or nov or void_cells or len(improved_servers) != len(cells):
    print(f"VERDICT VOID: {len(voids)} void run(s), {len(void_cells)} void cell(s), {len(nov)} metric(s) without a verdict")
else:
    print("VERDICT PASS")
EOF
