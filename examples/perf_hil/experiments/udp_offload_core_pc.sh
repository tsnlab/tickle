#!/usr/bin/env bash
# udp_offload_core_pc.sh - the core bench's p4 cells (core default build, TICKLE_P4_PATH=frag: a 2800 B sample as two
# DATA_FRAG datagrams) on this PC: the publisher's (client's) instructions per sample, user + kernel, before send
# offload (A = a5747a85), with f3c948ba's (B: every run of same-size datagrams as one UDP_SEGMENT send - here runs of
# 2), and with the candidate (C: send offload for large-sample fragments only, compiled out of a build without large
# samples). Two private namespaces joined by a veth whose UDP segmentation is off at both ends - the kernel cuts a run
# in software before the device, as on the Pi 5 (tx-udp-segmentation: off [fixed]) - each process with a private
# /dev/shm so samples cross the veth, as between the two Pis.
#
# NOT a CPU-time result: perf stat instruction counts on one PC and a veth. The rig A/B is what says what it costs.
#
# Cells (the rig's c4, c15, c17 shapes, short): thr = reliable_throughput p4 -Q -N 187 -B 100; be =
# best_effort_throughput p4; lat = reliable_latency p4 at -i 0.002 with a 512-ping warm-up and cool-down. Arms, in a
# rotated order every rep: A, A2 (A's binaries again: the A/A control), B, C.
#
# HOW TO READ IT (written before the first run, enforced below):
#   - a run counts only with a RESULT line from the client; lat only with loss_pct 0 (any loss is printed, per arm);
#   - per cell and arm, median over reps of client instructions / sent;
#   - noise = |A2 - A| per cell; C is "back to A" in a cell iff |C - A| <= max(noise, 1% of A); else C DIFFERS;
#   - B is printed beside them for the record: on the PC a run of 2 costs FEWER instructions (gso_run_cost.sh), so B
#     below A here does not contradict the rig's B above A - the rig's excess is not an instruction cost.
# Usage: REPS=5 udp_offload_core_pc.sh     (A, B, C are commits; C defaults to HEAD)   Launch detached; never the rig.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=box
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
A=${A:-a5747a85}
B=${B:-f3c948ba}
C=${C:-HEAD}
REPS=${REPS:-5}
DUR=${DUR:-3}
a_sha=$(git -C "$REPO" rev-parse --short "$A") || exit 1
b_sha=$(git -C "$REPO" rev-parse --short "$B") || exit 1
c_sha=$(git -C "$REPO" rev-parse --short "$C") || exit 1
OUT=${OUT:-$HOME/rig_results_safe/udp_offload_core_pc_${c_sha}_$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"
WORK=$(mktemp -d /tmp/uocore.XXXXXX) || exit 1
NSA=uocore-a-$$
NSB=uocore-b-$$
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
cleanup() {
    local ns pids
    for ns in "$NSA" "$NSB"; do
        pids=$(sudo -n ip netns pids "$ns" 2>/dev/null | tr '\n' ' ')
        # shellcheck disable=SC2086
        [ -n "${pids// /}" ] && sudo -n ip netns exec "$ns" kill -KILL $pids 2>/dev/null
        sudo -n ip netns del "$ns" 2>/dev/null
    done
    rm -rf "$WORK"
    return 0
}
trap cleanup EXIT

say "=== udp offload core p4, PC: A=$a_sha B=$b_sha C=$c_sha reps=$REPS dur=$DUR $(date -Is) ==="
CELLS="thr be lat"
scen_of() { case $1 in thr) echo reliable_throughput ;; be) echo best_effort_throughput ;; lat) echo reliable_latency ;; esac; }
args_of() {
    case $1 in
    thr) echo "-Q -N 187 -B 100 -d $DUR --warmup-s 0.5 --cooldown-s 0.5" ;;
    be) echo "-d $DUR --warmup-s 0.5 --cooldown-s 0.5" ;;
    lat) echo "-i 0.002 -d $DUR -W 512 -C 512 -I 0.001 -T 20" ;;
    esac
}
for arm in A B C; do
    rev=$a_sha
    [ "$arm" = B ] && rev=$b_sha
    [ "$arm" = C ] && rev=$c_sha
    mkdir -p "$WORK/src_$arm" "$WORK/home_$arm"
    git -C "$REPO" archive "$rev" | tar -x -C "$WORK/src_$arm" || { say "FATAL cannot export $rev"; exit 1; }
    for cell in $CELLS; do
        sc=$(scen_of "$cell")
        (cd "$WORK/src_$arm/examples/perf_hil/tickle" && HOME="$WORK/home_$arm" TICKLE_P4_PATH=frag ./build.sh "$sc" p4) \
            >"$WORK/build_${arm}_$cell.log" 2>&1 || { say "FATAL build $arm $cell"; tail -5 "$WORK/build_${arm}_$cell.log"; exit 1; }
        mkdir -p "$WORK/bin_${arm}_$cell"
        cp "$WORK/src_$arm/examples/perf_hil/tickle/${sc}_p4/client" "$WORK/src_$arm/examples/perf_hil/tickle/${sc}_p4/server" \
            "$WORK/bin_${arm}_$cell/"
        say "IDENTITY arm=$arm cell=$cell client=$(sha256sum "$WORK/bin_${arm}_$cell/client" | cut -c1-16)"
    done
done

if ! sudo -n ip netns add "$NSA" || ! sudo -n ip netns add "$NSB"; then say "FATAL netns"; exit 1; fi
sudo -n ip link add uocv1 netns "$NSA" type veth peer name uocv2 netns "$NSB" || { say "FATAL veth"; exit 1; }
sudo -n ip -n "$NSA" addr add 192.168.10.1/24 brd + dev uocv1
sudo -n ip -n "$NSB" addr add 192.168.10.2/24 brd + dev uocv2
for ns in "$NSA" "$NSB"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NSA" link set uocv1 up
sudo -n ip -n "$NSB" link set uocv2 up
sudo -n ip netns exec "$NSA" ethtool -K uocv1 tx-udp-segmentation off >/dev/null || { say "FATAL ethtool"; exit 1; }
sudo -n ip netns exec "$NSB" ethtool -K uocv2 tx-udp-segmentation off >/dev/null || { say "FATAL ethtool"; exit 1; }
say "veth tx-udp-segmentation: $(sudo -n ip netns exec "$NSA" ethtool -k uocv1 | sed -n 's/^tx-udp-segmentation: //p') /" \
    "$(sudo -n ip netns exec "$NSB" ethtool -k uocv2 | sed -n 's/^tx-udp-segmentation: //p')"

run_cell() { # rep cell arm
    local rep=$1 cell=$2 arm=$3 bin_arm=$3
    [ "$arm" = A2 ] && bin_arm=A
    local bin="$WORK/bin_${bin_arm}_$cell" args
    args=$(args_of "$cell")
    # The server's own PID, written by the shell that then becomes it (exec), so it is stopped by PID, never by name.
    rm -f "$WORK/srv.pid"
    # shellcheck disable=SC2016,SC2086,SC2024 # inner sh's $1/$@; args split on purpose; logs are ours
    sudo -n ip netns exec "$NSB" sh -c 'mount -t tmpfs -o size=256m tmpfs /dev/shm && cd "$1" && echo $$ > "$2" && shift 2 && exec "$@"' \
        sh "$bin" "$WORK/srv.pid" env BENCH_IFACE=uocv2 ./server $args >"$WORK/srv.log" 2>&1 </dev/null &
    local srv=$!
    sleep 1
    # shellcheck disable=SC2016,SC2086,SC2024
    sudo -n ip netns exec "$NSA" sh -c 'mount -t tmpfs -o size=256m tmpfs /dev/shm && cd "$1" && shift && exec "$@"' \
        sh "$bin" env BENCH_IFACE=uocv1 timeout 90 perf stat -x, -e instructions:u,instructions:k -o "$WORK/cli.csv" -- \
        ./client $args >"$WORK/cli.log" 2>&1 </dev/null
    local spid
    spid=$(cat "$WORK/srv.pid" 2>/dev/null)
    case "$(sudo -n ip netns exec "$NSB" readlink "/proc/$spid/exe" 2>/dev/null)" in
    "$bin/server") sudo -n ip netns exec "$NSB" kill -INT "$spid" ;;
    *) say "rep=$rep cell=$cell arm=$arm note: server $spid gone before SIGINT" ;;
    esac
    wait "$srv"
    local res insns
    res=$(grep -m1 '^RESULT' "$WORK/cli.log")
    insns=$(awk -F, '$3 ~ /^instructions/ { s += $1 } END { printf "%.0f", s }' "$WORK/cli.csv" 2>/dev/null)
    if [ -z "$res" ]; then
        say "rep=$rep cell=$cell arm=$arm VOID no client RESULT: $(tail -2 "$WORK/cli.log" | tr '\n' ' ')"
        return
    fi
    local f
    f=$(tr ' ' '\n' <<<"$res" | grep -E '^(sent|recv|loss_pct|rtt_avg_ms|send_mbps|gso_sends|udp_offload)=' | tr '\n' ' ')
    # Treatment, from the client's traffic line (absent before the offload change): B must have sent runs, A and C none.
    local gso
    gso=$(grep -o 'gso_sends=[0-9]*' "$WORK/cli.log" | tail -1 | cut -d= -f2)
    gso=${gso:-0}
    case "$arm:$gso" in
    B:0) say "rep=$rep cell=$cell arm=$arm VOID_treatment gso_sends=0 (B must send runs)"; return ;;
    B:*) ;;
    *:0) ;;
    *) say "rep=$rep cell=$cell arm=$arm VOID_treatment gso_sends=$gso (A and C must send none)"; return ;;
    esac
    say "rep=$rep cell=$cell arm=$arm client_insns=$insns $f gso_sends=$gso"
}

ARMS=(A A2 B C)
for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep/$REPS $(date -Is) ---"
    for cell in $CELLS; do
        for k in 0 1 2 3; do run_cell "$rep" "$cell" "${ARMS[$(((k + rep) % 4))]}"; done
    done
done
say "=== runs done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
v = {}
loss = {}
for line in open(sys.argv[1]):
    m = re.match(r"rep=(\d+) cell=(\S+) arm=(\S+) client_insns=(\d+) (.*)", line)
    if not m:
        continue
    f = dict(kv.split("=", 1) for kv in m.group(5).split() if "=" in kv)
    cell, arm = m.group(2), m.group(3)
    sent = float(f.get("sent", "0"))
    if cell == "lat" and float(f.get("loss_pct", "0")) != 0:
        loss.setdefault((cell, arm), []).append(f.get("loss_pct"))
        continue
    if sent > 0:
        v.setdefault((cell, arm), []).append(int(m.group(4)) / sent)
for k, l in sorted(loss.items()):
    print(f"LOSS {k[0]} {k[1]}: loss_pct {l} (runs excluded)")
for cell in ("thr", "be", "lat"):
    med = {a: st.median(v[(cell, a)]) for a in ("A", "A2", "B", "C") if len(v.get((cell, a), [])) >= 3}
    if len(med) < 4:
        print(f"{cell}: NO READING (fewer than 3 usable reps in an arm: { {a: len(v.get((cell, a), [])) for a in ('A','A2','B','C')} })")
        continue
    noise = abs(med["A2"] - med["A"])
    tol = max(noise, 0.01 * med["A"])
    c = med["C"] - med["A"]
    verdict = "BACK TO A" if abs(c) <= tol else "C DIFFERS"
    print(f"{cell}: client insns/sample A {med['A']:.0f} A2 {med['A2']:.0f} (A/A {100*(med['A2']-med['A'])/med['A']:+.2f}%) "
          f"B {med['B']:.0f} ({100*(med['B']-med['A'])/med['A']:+.2f}%) C {med['C']:.0f} ({100*c/med['A']:+.2f}%) "
          f"tolerance {100*tol/med['A']:.2f}% -> {verdict}")
PYEOF
say "=== udp_offload_core_pc done $(date -Is) out=$OUT ==="
