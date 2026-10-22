#!/usr/bin/env bash
# rx_uring_wait_pc.sh - the wait ring (hal_linux.c, "The wait ring", ab/rx-uring-dt) on THIS PC: instructions and cycles,
# user and kernel, per sample of the core ping-pong and stream cells, against main and a placement control. 2026-10-22.
# Derived from initoptions_insn_pc.sh (its build, namespaces and placement arm); what it adds is the kernel side, which
# is where the ring acts (one io_uring_enter() where epoll_pwait2() + recvfrom() were two system calls), the cycles,
# and the treatment check (ring_reads on the contexts' rx lines).
#
# Arms: label=<commit|@dir>[+P], as initoptions_insn_pc.sh. +P: gate v2's placement flags (every function moved by a
# multiple of 64 bytes): P = A+P is the floor a B-vs-A difference must exceed. An arm built with the wait ring on is the
# branch's head (tt_RX_URING_WAIT 1); a build where it is off shows ring=-1 (the treatment check below).
#
# Cells: b1 best_effort_latency p1 (the ping-pong the rig's prod/prod_dt arms model: every wait ends with a datagram);
# p1 reliable_latency p1; t1 reliable_throughput p1 (-Q -N 2048 -B 100, a stream: the receiver's waits mostly end
# for data, its drain reads most datagrams). Per sample = per client sent=.
#
# PRE-REGISTERED READING (the python at the end):
#   VOID   a run without all four perf counts or without the client's RESULT line with sent= > 0.
#   TREATMENT  per arm and cell, the contexts' ring_reads summed from the rx lines (client and server); an arm whose
#          label starts with B and reads nothing by the ring is reported "UNTREATED" and its rows are not read.
#   per arm, cell, role and metric (insn_u, insn_k, cycles_u+k a sample): mean, spread (min-max in %), delta to the
#          FIRST arm (A) in percent; floor = |mean(P) - mean(A)| in percent.
#   EQUAL   |delta| < max(0.3%, 3 x A's own spread%, 1.5 x floor); WORSE / BETTER beyond it.
#   This PC is shared and not the rig: the reading is reported, the rig decides (rig_queue_rx_uring_wait.sh).
#
# Usage: ARMS="A=origin/main P=origin/main+P B=ab/rx-uring-dt" [REPS=3] [CELLS="b1 p1 t1"] [CPU_C=12] [CPU_S=13]
#        [OUT=prefix] rx_uring_wait_pc.sh
#   OUT defaults to ~/rig_results_safe/rx_uring_wait_pc_<stamp> ($OUT.txt, $OUT.d/). Needs `sudo -n ip`. Launch
#   detached (setsid nohup); the summary always ends with "=== rx_uring_wait_pc ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
ARMS=${ARMS:?ARMS=\"label=commit|@dir ...\"}
REPS=${REPS:-3}
CELLS=${CELLS:-"b1 p1 t1"}
PLACEMENT=${PLACEMENT:-P}
CPU_C=${CPU_C:-12}
CPU_S=${CPU_S:-13}
CLIENT_ARGS=${CLIENT_ARGS:-}
OUT=${OUT:-$HOME/rig_results_safe/rx_uring_wait_pc_$(date +%Y%m%d-%H%M%S)}
# shellcheck disable=SC2034 # read by gate_v2_lib.sh
GATE_BUILD_ARCH=$(uname -m) # the arms build here, not on a Pi
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091
. "$HERE/gate_v2_lib.sh"
layout_cflags_default
T0=$(date +%s)
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
WORK=$(mktemp -d "$HOME/tmp_build/rx_uring_wait_pc.XXXXXX" 2>/dev/null || mktemp -d /tmp/rx_uring_wait_pc.XXXXXX) || exit 1
mkdir -p "$WORK/tmp" && export TMPDIR="$WORK/tmp"
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
on_exit() {
    local rc=$? ns
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== rx_uring_wait_pc ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
say "=== rx_uring_wait_pc $(date -Is): ARMS=$ARMS REPS=$REPS CELLS=$CELLS CLIENT_ARGS='$CLIENT_ARGS' TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS' harness=$(git -C "$REPO" rev-parse --short HEAD) uptime='$(uptime)' ==="

DUR_T=${DUR_T:-3}
bench_of() { case $1 in b*) echo best_effort_latency ;; p*) echo reliable_latency ;; t*) echo reliable_throughput ;; esac; }
args_of() {
    case $1 in
    b* | p*) echo "$CLIENT_ARGS" ;;
    t1) echo "-Q -N 2048 -B 100 -d $DUR_T --warmup-s 1 --cooldown-s 1" ;;
    t3) echo "-Q -N 368 -B 100 -d $DUR_T --warmup-s 1 --cooldown-s 1" ;;
    esac
}
LABELS=()
for a in $ARMS; do
    label=${a%%=*}
    src=${a#*=}
    flags=$TICKLE_EXTRA_CFLAGS
    if [ "${src%+P}" != "$src" ]; then
        src=${src%+P}
        flags=$TICKLE_PLACEMENT_CFLAGS
    fi
    LABELS+=("$label")
    tree="$WORK/$label/src"
    log="$OUT.d/build_$label.log"
    mkdir -p "$tree" "$WORK/$label/home"
    if [ "${src:0:1}" = @ ]; then
        (cd "${src:1}" && git ls-files -z | tar --null -T - -cf -) | tar -x -C "$tree"
        echo "source: tree ${src:1} at $(git -C "${src:1}" rev-parse HEAD) + diff $(git -C "${src:1}" diff HEAD | sha256sum | cut -c1-12)" >"$log"
    else
        git -C "$REPO" archive "$src" | tar -x -C "$tree"
        echo "source: commit $(git -C "$REPO" rev-parse "$src")" >"$log"
    fi
    for p in $CELLS; do
        b=$(bench_of "$p")
        (cd "$tree/examples/perf_hil/tickle" && HOME="$WORK/$label/home" TICKLE_EXTRA_CFLAGS="$flags" nice -n 19 ./build.sh "$b" "p${p#?}") \
            >>"$log" 2>&1 || {
            say "FATAL: build $label $p failed - $log"
            exit 1
        }
        mkdir -p "$WORK/bin/$label/$p"
        cp "$tree/examples/perf_hil/tickle/${b}_p${p#?}/client" "$tree/examples/perf_hil/tickle/${b}_p${p#?}/server" \
            "$WORK/bin/$label/$p/"
    done
    say "    built $label ($src, flags '$flags'): $(sed -n 1p "$log") in $(($(date +%s) - T0)) s"
done

EVENTS=instructions:u,instructions:k,cycles:u,cycles:k
NSID=0
run_one() { # $1 rep, $2 arm, $3 cell
    local rep=$1 arm=$2 p=$3 f d
    f="$OUT.d/r${rep}_${arm}_${p}"
    NSID=$((NSID + 1))
    local na="ruw$$-${NSID}a" nb="ruw$$-${NSID}b" va="ruw$$v${NSID}a" vb="ruw$$v${NSID}b"
    echo "$na" >>"$NS_LIST"
    echo "$nb" >>"$NS_LIST"
    if ! { sudo -n ip netns add "$na" && sudo -n ip netns add "$nb" && sudo -n ip link add "$va" type veth peer name "$vb" &&
        sudo -n ip link set "$va" netns "$na" && sudo -n ip link set "$vb" netns "$nb" &&
        sudo -n ip -n "$na" addr add 192.168.10.1/24 brd + dev "$va" && sudo -n ip -n "$nb" addr add 192.168.10.2/24 brd + dev "$vb" &&
        sudo -n ip -n "$na" link set lo up && sudo -n ip -n "$nb" link set lo up &&
        sudo -n ip -n "$na" link set "$va" up && sudo -n ip -n "$nb" link set "$vb" up; }; then
        echo "no veth" >"$f.note"
        return 0
    fi
    d="$WORK/bin/$arm/$p"
    # shellcheck disable=SC2024 # the logs are this user's, written by this shell
    sudo -n ip netns exec "$nb" sh -c "mount -t tmpfs -o size=64m ruwshm /dev/shm || exit 9
cd $d && env BENCH_IFACE=$vb nice -n 19 taskset -c $CPU_S perf stat -x, -o $WORK/s.perf -e $EVENTS -- ./server $(case $p in t*) args_of "$p" ;; esac); true" \
        >"$f.server.log" 2>&1 &
    local sp=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$na" sh -c "mount -t tmpfs -o size=64m ruwshm /dev/shm || exit 9
cd $d && env BENCH_IFACE=$va nice -n 19 taskset -c $CPU_C perf stat -x, -o $WORK/c.perf -e $EVENTS -- ./client $(args_of "$p"); echo client_rc=\$?; true" \
        >"$f.client.log" 2>&1
    sleep 1
    local pid
    for pid in $(sudo -n ip netns pids "$nb"); do
        [ "$(sudo -n ip netns exec "$nb" readlink "/proc/$pid/exe" 2>/dev/null)" = "$d/server" ] &&
            sudo -n ip netns exec "$nb" kill -INT "$pid"
    done
    wait $sp
    sudo -n ip netns del "$na"
    sudo -n ip netns del "$nb"
    [ -f "$WORK/c.perf" ] && cp "$WORK/c.perf" "$f.client.perf" && rm -f "$WORK/c.perf"
    [ -f "$WORK/s.perf" ] && cp "$WORK/s.perf" "$f.server.perf" && rm -f "$WORK/s.perf"
    echo "$(date +%T) r$rep $arm $p $(grep -m1 '^RESULT' "$f.client.log" | tr ' ' '\n' | grep -E '^(sent|rtt_avg_ms|rtt_p50_us)=' | tr '\n' ' ') $(grep -ho 'ring=[-0-9]* ring_waits=[0-9]* ring_reads=[0-9]*' "$f.client.log" "$f.server.log" | tr '\n' ' ')" >>"$SUM"
}
for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep $(date -Is) uptime='$(uptime)' ---"
    order=("${LABELS[@]}")
    if [ $((rep % 2)) = 0 ]; then
        order=()
        for ((i = ${#LABELS[@]} - 1; i >= 0; i--)); do order+=("${LABELS[$i]}"); done
    fi
    for p in $CELLS; do
        for arm in "${order[@]}"; do
            run_one "$rep" "$arm" "$p"
        done
    done
done
say "--- runs done at $(($(date +%s) - T0)) s ---"
python3 - "$OUT.d" "$PLACEMENT" "${LABELS[@]}" <<'PY' | tee -a "$SUM"
import os, re, sys, statistics as st
d, placement, labels = sys.argv[1], sys.argv[2], sys.argv[3:]
METRICS = ("insn_u", "insn_k", "cycles")
def perf(path):
    got = {}
    try:
        for line in open(path):
            f = line.strip().split(",")
            if len(f) > 2 and f[0].isdigit():
                got[f[2].split("/")[0]] = int(f[0])
    except OSError:
        return None
    need = ("instructions:u", "instructions:k", "cycles:u", "cycles:k")
    if any(k not in got for k in need):
        return None
    return {"insn_u": got["instructions:u"], "insn_k": got["instructions:k"], "cycles": got["cycles:u"] + got["cycles:k"]}
def ring_reads(*paths):
    total = 0
    for path in paths:
        try:
            total += sum(int(x) for x in re.findall(r"\bring_reads=(\d+)", open(path, errors="replace").read()))
        except OSError:
            pass
    return total
runs, treated, void = {}, {}, 0
for name in sorted(os.listdir(d)):
    m = re.match(r"r(\d+)_(.+)_([bpt]\d)\.client\.log$", name)
    if not m:
        continue
    rep, arm, p = m.groups()
    base = os.path.join(d, f"r{rep}_{arm}_{p}")
    text = open(base + ".client.log", errors="replace").read()
    s = re.search(r"^RESULT.*\bsent=(\d+)", text, re.M)
    c, sv = perf(base + ".client.perf"), perf(base + ".server.perf")
    if not s or int(s.group(1)) == 0 or c is None or sv is None:
        void += 1
        print(f"VOID r{rep} {arm} {p}: sent={s.group(1) if s else None} perf client={c is not None} server={sv is not None}")
        continue
    n = int(s.group(1))
    runs.setdefault((arm, p), []).append(({k: c[k] / n for k in METRICS}, {k: sv[k] / n for k in METRICS}, n))
    treated[(arm, p)] = treated.get((arm, p), 0) + ring_reads(base + ".client.log", base + ".server.log")
print("\nTREATMENT (ring_reads, client + server, all reps):")
untreated = set()
for (arm, p), reads in sorted(treated.items()):
    flag = ""
    if arm.startswith("B") and reads == 0:
        flag = "  UNTREATED - not read"
        untreated.add((arm, p))
    print(f"  {p} {arm}: {reads}{flag}")
print(f"\n{'cell':4} {'arm':>4} {'role':>6} {'metric':>7} {'n':>2} {'mean/sample':>12} {'spread%':>8} {'d%':>7} verdict")
for p in sorted({k[1] for k in runs}):
    base = runs.get((labels[0], p))
    for role, ri in (("client", 0), ("server", 1)):
        for metric in METRICS:
            for arm in labels:
                r = runs.get((arm, p))
                if not r or (arm, p) in untreated:
                    continue
                v = [x[ri][metric] for x in r]
                m = st.mean(v)
                spread = (max(v) - min(v)) / m * 100
                line = f"{p:4} {arm:>4} {role:>6} {metric:>7} {len(r):>2} {m:>12.1f} {spread:>8.3f}"
                if base and arm != labels[0]:
                    bv = [x[ri][metric] for x in base]
                    bm = st.mean(bv)
                    bs = (max(bv) - min(bv)) / bm * 100
                    dp = (m - bm) / bm * 100
                    pr = runs.get((placement, p))
                    floor = abs(st.mean([x[ri][metric] for x in pr]) - bm) / bm * 100 if pr else 0.0
                    bound = max(0.3, 3 * bs, 1.5 * floor)
                    tag = "EQUAL" if abs(dp) < bound else ("WORSE" if dp > 0 else "BETTER")
                    line += f" {dp:>+7.2f} {tag}(<{bound:.2f}%)"
                print(line)
print(f"\nvoid runs: {void}")
PY
