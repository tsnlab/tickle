#!/usr/bin/env bash
# initoptions_insn_pc.sh - user instructions per sample of the reliable cells, with and without loss, on THIS PC, for any
# number of arms, each optionally rebuilt with gate v2's placement flags. 2026-10-21, for tt_InitOptions bundle 3 (the
# reliable group: a struct field load where the repair path read an immediate). Derived from initoptions_insn_pc.sh, whose
# cells, namespaces, build and metric it keeps; what it adds is the placement arm and the loss cells.
#
# Arms: label=<commit|@dir>[+P]. +P builds that source with TICKLE_PLACEMENT_CFLAGS (gate_v2_lib.sh A3: every function
# moved by a multiple of 64 bytes, nothing else) instead of the layout flags: P = A+P is the placement control, and
# |P - A| is the floor a B-vs-A difference must exceed.
#
# Cells: pN reliable_latency pN (gate v2 c10 c11 c16 c17); tN reliable_throughput pN as gate v2 c1-c4 (-Q -N .. -B 100,
# -d $DUR_T); lN the tN cell with netem loss LOSS_PCT (default 1) on both ends of the veth - the repair path: ACKNACK
# retries, the back-off, the writer's resends and their suppression, which read the group's fields.
#
# The metric is the gate's: a whole process's instructions:u divided by the client's samples (sent=).
#
# PRE-REGISTERED READING (the python at the end):
#   VOID  a run without both perf counts or without the client's RESULT line with sent= > 0.
#   per arm and cell: mean and min-max of insn_u/sample, client and server; delta to the FIRST arm (A) in percent.
#   floor: |mean(P) - mean(A)| in percent for the arm named by PLACEMENT (default "P"), per cell and role.
#   EQUAL   |delta| < max(0.3%, 3 x A's own spread%, 1.5 x floor) - the placement floor and the repetition spread are
#           what a layout or a random loss pattern alone moves; within them nothing is shown.
#   WORSE / BETTER  beyond that bound. A loss cell's count depends on which datagrams the random loss hits, so its own
#           spread is expected to be wider than a lossless cell's; the bound widens with it, by rule, not by eye.
#
# Usage: ARMS="A=origin/main P=origin/main+P B=ab/initoptions-3-reliable" [REPS=3] [CELLS="p1 p3 t1 t3 l1 l3"]
#        [LOSS_PCT=1] [CPU_C=12] [CPU_S=13] [OUT=prefix] initoptions_insn_pc.sh
#   OUT defaults to ~/rig_results_safe/initoptions_insn_pc_<stamp> ($OUT.txt, $OUT.d/). Needs `sudo -n ip` (tc runs
#   under `ip netns exec`). Launch detached (setsid nohup); the summary always ends with "=== initoptions_insn_pc ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
ARMS=${ARMS:?ARMS=\"label=commit|@dir ...\"}
REPS=${REPS:-3}
CELLS=${CELLS:-"p1 p3 t1 t3 l1 l3"}
LOSS_PCT=${LOSS_PCT:-1}
PLACEMENT=${PLACEMENT:-P}
CPU_C=${CPU_C:-12}
CPU_S=${CPU_S:-13}
CLIENT_ARGS=${CLIENT_ARGS:-}
# RECORD=1: also `perf record -e instructions:u` (period 10007) of each process, reported by symbol into $OUT.d - for a
# function-level instruction diff. The perf stat counts then include perf record itself: read only the reports.
RECORD=${RECORD:-0}
OUT=${OUT:-$HOME/rig_results_safe/initoptions_insn_pc_$(date +%Y%m%d-%H%M%S)}
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
WORK=$(mktemp -d "$HOME/tmp_build/initoptions_insn_pc.XXXXXX" 2>/dev/null || mktemp -d /tmp/initoptions_insn_pc.XXXXXX) || exit 1
# The compilers' temporaries too: /tmp has a per-user quota that other sessions fill (a build died on it, 2026-10-19).
mkdir -p "$WORK/tmp" && export TMPDIR="$WORK/tmp"
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
on_exit() {
    local rc=$? ns
    while read -r ns; do
        [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== initoptions_insn_pc ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
say "=== initoptions_insn_pc $(date -Is): ARMS=$ARMS REPS=$REPS CELLS=$CELLS CLIENT_ARGS='$CLIENT_ARGS' TICKLE_EXTRA_CFLAGS='$TICKLE_EXTRA_CFLAGS' harness=$(git -C "$REPO" rev-parse --short HEAD) ==="

# pN: reliable_latency pN, no arguments (gate v2 c10 c11 c16 c17). tN: reliable_throughput pN as gate v2 c1-c4,
# -Q -N <2048|405|368|187> -B 100, with -d $DUR_T --warmup-s 1 --cooldown-s 1 here; per sample = per client sent=.
DUR_T=${DUR_T:-3}
bench_of() { case $1 in p*) echo reliable_latency ;; t* | l*) echo reliable_throughput ;; esac; }
args_of() {
    case $1 in
    p*) echo "$CLIENT_ARGS" ;;
    t1) echo "-Q -N 2048 -B 100 -d $DUR_T --warmup-s 1 --cooldown-s 1" ;;
    t2) echo "-Q -N 405 -B 100 -d $DUR_T --warmup-s 1 --cooldown-s 1" ;;
    t3) echo "-Q -N 368 -B 100 -d $DUR_T --warmup-s 1 --cooldown-s 1" ;;
    t4) echo "-Q -N 187 -B 100 -d $DUR_T --warmup-s 1 --cooldown-s 1" ;;
    l*) args_of "t${1#l}" ;;
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

perf_prefix() { # $1 c or s
    local pre="perf stat -x, -o $WORK/$1.perf -e instructions:u,instructions:k --"
    [ "$RECORD" = 1 ] && pre="$pre perf record -q -e instructions:u -c ${REC_PERIOD:-10007} -o $WORK/$1.data --"
    echo "$pre"
}
NSID=0
run_one() { # $1 rep, $2 arm, $3 cell
    local rep=$1 arm=$2 p=$3 f d
    f="$OUT.d/r${rep}_${arm}_${p}"
    NSID=$((NSID + 1))
    local na="qip$$-${NSID}a" nb="qip$$-${NSID}b" va="qip$$v${NSID}a" vb="qip$$v${NSID}b"
    echo "$na" >>"$NS_LIST"
    echo "$nb" >>"$NS_LIST"
    if ! { sudo -n ip netns add "$na" && sudo -n ip netns add "$nb" && sudo -n ip link add "$va" type veth peer name "$vb" &&
        sudo -n ip link set "$va" netns "$na" && sudo -n ip link set "$vb" netns "$nb" &&
        sudo -n ip -n "$na" addr add 192.168.10.1/24 brd + dev "$va" && sudo -n ip -n "$nb" addr add 192.168.10.2/24 brd + dev "$vb" &&
        sudo -n ip -n "$na" link set lo up && sudo -n ip -n "$nb" link set lo up &&
        sudo -n ip -n "$na" link set "$va" up && sudo -n ip -n "$nb" link set "$vb" up &&
        case $p in
        l*) sudo -n ip netns exec "$na" tc qdisc add dev "$va" root netem loss "${LOSS_PCT}%" &&
            sudo -n ip netns exec "$nb" tc qdisc add dev "$vb" root netem loss "${LOSS_PCT}%" ;;
        *) true ;;
        esac; }; then
        echo "no veth" >"$f.note"
        return 0
    fi
    d="$WORK/bin/$arm/$p"
    # shellcheck disable=SC2024 # the logs are this user's, written by this shell
    sudo -n ip netns exec "$nb" sh -c "mount -t tmpfs -o size=64m qipshm /dev/shm || exit 9
cd $d && env BENCH_IFACE=$vb nice -n 19 taskset -c $CPU_S $(perf_prefix s) ./server $(case $p in t* | l*) args_of "$p" ;; esac); chmod a+r $WORK/s.data 2>/dev/null; true" \
        >"$f.server.log" 2>&1 &
    local sp=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$na" sh -c "mount -t tmpfs -o size=64m qipshm /dev/shm || exit 9
cd $d && env BENCH_IFACE=$va nice -n 19 taskset -c $CPU_C $(perf_prefix c) ./client $(args_of "$p"); echo client_rc=\$?; chmod a+r $WORK/c.data 2>/dev/null; true" \
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
    local r
    for r in c:client s:server; do
        [ -f "$WORK/${r%%:*}.data" ] || continue
        perf report -f -i "$WORK/${r%%:*}.data" --no-children -q -F sample,sym --stdio 2>/dev/null >"$f.${r#*:}.report"
        rm -f "$WORK/${r%%:*}.data"
    done
    echo "$(date +%T) r$rep $arm $p $(grep -m1 '^RESULT' "$f.client.log" | tr ' ' '\n' | grep -E '^(sent|measured|rtt_p50_ms)=' | tr '\n' ' ')" >>"$SUM"
}
for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep $(date -Is) ---"
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
def perf_u(path):
    try:
        for line in open(path):
            f = line.strip().split(",")
            if len(f) > 2 and f[2].startswith("instructions:u") and f[0].isdigit():
                return int(f[0])
    except OSError:
        pass
    return None
runs = {}
void = 0
for name in sorted(os.listdir(d)):
    m = re.match(r"r(\d+)_(.+)_([ptl]\d)\.client\.log$", name)
    if not m:
        continue
    rep, arm, p = m.groups()
    text = open(os.path.join(d, name), errors="replace").read()
    s = re.search(r"^RESULT.*\bsent=(\d+)", text, re.M)
    cu = perf_u(os.path.join(d, f"r{rep}_{arm}_{p}.client.perf"))
    su = perf_u(os.path.join(d, f"r{rep}_{arm}_{p}.server.perf"))
    if not s or int(s.group(1)) == 0 or cu is None or su is None:
        void += 1
        print(f"VOID r{rep} {arm} {p}: sent={s.group(1) if s else None} client_u={cu} server_u={su}")
        continue
    n = int(s.group(1))
    runs.setdefault((arm, p), []).append((cu / n, su / n, n))
print(f"\n{'cell':4} {'arm':>10} {'n':>2} {'rtts':>6} {'client u/rtt':>13} {'spread%':>8} {'d%':>7} {'server u/rtt':>13} {'spread%':>8} {'d%':>7}")
for p in sorted({k[1] for k in runs}):
    base = runs.get((labels[0], p))
    for arm in labels:
        r = runs.get((arm, p))
        if not r:
            print(f"{p:4} {arm:>10} no runs")
            continue
        out = f"{p:4} {arm:>10} {len(r):>2} {r[0][2]:>6}"
        for i in (0, 1):
            v = [x[i] for x in r]
            m = st.mean(v)
            spread = (max(v) - min(v)) / m * 100
            if base and arm != labels[0]:
                bv = [x[i] for x in base]
                bm = st.mean(bv)
                bs = (max(bv) - min(bv)) / bm * 100
                dp = (m - bm) / bm * 100
                pr = runs.get((placement, p))
                floor = abs(st.mean([x[i] for x in pr]) - bm) / bm * 100 if pr else 0.0
                bound = max(0.3, 3 * bs, 1.5 * floor)
                tag = "EQUAL" if abs(dp) < bound else ("WORSE" if dp > 0 else "BETTER")
                tag += f"(<{bound:.2f}%)"
                out += f" {m:>13.1f} {spread:>8.3f} {dp:>+7.2f} {tag}"
            else:
                out += f" {m:>13.1f} {spread:>8.3f} {'':>7}"
        print(out)
print(f"\nvoid runs: {void}")
PY
