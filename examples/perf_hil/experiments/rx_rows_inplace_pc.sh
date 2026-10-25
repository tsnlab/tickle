#!/usr/bin/env bash
# rx_rows_inplace_pc.sh - ROADMAP "in-place recvmmsg rows" (old OPTIMIZATION_PLAN 12.2, step 0): what the copy of a
# recvmmsg batch's datagrams 2..N out of struct tt_hal.rx_batch into the context's receive buffer
# (hal_linux.c rx_take_pending()) costs the receiving server per sample, on THIS PC, over the UDP path, in a private
# network namespace. Written 2026-10-25 before any of its runs, and before the prototype it reads existed.
# A PC figure for deciding whether a rig A/B is worth running; never a published figure.
#
# Usage: rx_rows_inplace_pc.sh A_DIR A2_DIR B_DIR      (each built by rx_rows_inplace_build.sh)
#   A   origin/main's receive path (copy of rows 2..N)
#   A2  the SAME binaries as A copied to another directory: the A/A control. Nothing the prototype changes can touch it,
#       so A2 - A is the noise and drift floor of this PC and this run order.
#   B   the prototype: rows 2..N handed to core where they lie (no copy) when no lending pool is attached.
# Env: REPS (6), DUR (5 s measured, plus 2 s warm-up and 2 s cool-down), PIN ("2 4": server, client CPUs),
#      OUT (default ~/rxip_results/rx_rows_inplace_<date>.txt - outside any session directory), CELLS (all).
# The last line is always "RX_ROWS_INPLACE_DONE rc=<n> out=<file>".
#
# CELLS (one netns, a dummy link carrying 192.168.10.255, the segment compiled out so the pair talks UDP):
#   be_p1_max    best_effort_throughput p1 (76 B sample), client at max rate
#   be_p2_max    best_effort_throughput p2 (1292 B), max rate
#   be_p2_paced  best_effort_throughput p2, -i 11 us (~90k samples/s, the rig's P2 server rate: 453k in 5 s)
#   rel_p2_max   reliable_throughput p2 (RELIABLE KEEP_LAST), max rate
# Per run: perf stat on the server for its whole life - cycles:u, instructions:u, cycles:k, instructions:k, task-clock
# - divided by the server's recv=; and the server's rx_batch_calls/rx_batch_datagrams, from which the share of samples
# that went through the copied rows is (datagrams - calls) / recv (an upper bound: discovery datagrams are counted too).
#
# READING RULES (pre-registered; implemented in the Python below, not only stated here):
#   VOID run: a client or server RESULT missing, a window not ok, perf output missing, recv < 1000, or for a BE cell
#     recv < 50% of the client's sent= (a server that drops half is in another regime). VOID runs are listed, not read.
#   A cell is READABLE only with >= 4 non-void runs in each of A, A2 and B.
#   Per cell, per metric m in {cyc_u, ins_u, task_us} per received sample: delta_B = median(B)/median(A) - 1 and
#     delta_AA = median(A2)/median(A) - 1. The A/A floor is max(|delta_AA|, the A arm's own half-range/median).
#   Mechanism check (instructions): the copy's predicted saving per sample is rows_share x the instructions of one
#     memcpy of the cell's datagram size on this machine (measured in the same run by the memcpy probe below, glibc's
#     own memcpy, the same call hal_linux.c makes). B's ins_u drop must be within 0.5x..2x of that prediction; outside
#     it, whatever moved is not the copy and the cell reads NOT-THE-COPY. (x86 rep movsb counts one instruction for any
#     length, so an instruction count can understate a copy; the probe measures what this glibc actually executes at
#     this size, and cycles are the primary reading.)
#   Verdict per cell on cyc_u:  SAVES   delta_B <= -(floor + 1%) and the mechanism check holds
#                               SAME    |delta_B| <= floor + 1%
#                               WORSE   delta_B >= +(floor + 1%)
#   Dose control: the copied bytes per sample at p1 are ~1/12 of p2's, so a SAVES at p1 as large as p2's (ratio of
#     cycle savings p1/p2 > 0.5) says the saving is not proportional to the bytes - it is layout or the branch, and the
#     p2 saving is not credited to the copy either: the run reads NOT-BYTES-PROPORTIONAL.
#   RIG A/B WORTH RUNNING only when be_p2_max AND be_p2_paced both read SAVES with cyc_u <= -2%, rel_p2_max is not
#     WORSE, no cell's recv falls (B's median recv >= 99% of A's in every cell), and the dose control passes. Otherwise
#     the ROADMAP item is closed with the numbers: the copy is not where the receive cost is.
#   What would falsify "the row copy is a material cost": be_p2_max SAME or NOT-THE-COPY.
set -uo pipefail
[ $# -eq 3 ] || { echo "usage: $0 A_DIR A2_DIR B_DIR" >&2; exit 2; }
REPS=${REPS:-6}
DUR=${DUR:-5}
PIN=${PIN-2 4}
CELLS=${CELLS:-be_p1_max be_p2_max be_p2_paced rel_p2_max}
OUT=${OUT:-$HOME/rxip_results/rx_rows_inplace_$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"
: >"$OUT"
NS=tickle_rxip_$$
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rx_rows_inplace.XXXXXX")
rc=1
finish() {
    sudo -n ip netns del "$NS" 2>/dev/null
    echo "RX_ROWS_INPLACE_DONE rc=$rc out=$OUT" | tee -a "$OUT"
}
trap finish EXIT
say() { echo "$*" | tee -a "$OUT"; }
ARMS=("$@")
NAMES=(A A2 B)

# The memcpy probe: instructions:u and cycles:u of glibc's memcpy at each cell's datagram size, hot, per call.
cat >"$WORK/probe.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char src[1536] __attribute__((aligned(64))), dst[1536 * 2] __attribute__((aligned(64)));
int main(int argc, char** argv) {
    size_t n = (size_t)atol(argv[1]);
    long iters = atol(argv[2]);
    void* (*volatile mc)(void*, const void*, size_t) = memcpy;
    for (long i = 0; i < iters; i++) mc(dst + 8, src, n);
    return dst[9];
}
EOF
gcc -O2 -o "$WORK/probe" "$WORK/probe.c" || exit
for n in 0 110 1320; do # 0: the loop and call alone, subtracted; 110 and 1320: p1's and p2's datagrams
    perf stat -x, -e instructions:u,cycles:u -o "$WORK/probe_$n.txt" "$WORK/probe" "$n" 10000000 || exit
    say "PROBE bytes=$n $(grep -E 'instructions|cycles' "$WORK/probe_$n.txt" | cut -d, -f1,3 | tr '\n' ' ')"
done

sudo -n ip netns add "$NS" || { say "FATAL cannot create netns"; exit; }
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" route add default dev lo
sudo -n ip -n "$NS" link add bench0 type dummy
sudo -n ip -n "$NS" addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip -n "$NS" link set bench0 up

say "=== rx_rows_inplace_pc $(date -Is) reps=$REPS dur=$DUR pin='$PIN' cells='$CELLS' load='$(cut -d' ' -f1-3 /proc/loadavg)'"
for i in 0 1 2; do
    say "arm ${NAMES[$i]} dir=${ARMS[$i]} $(head -1 "${ARMS[$i]}/source.txt" 2>/dev/null)" \
        "be_p2_server=$(sha256sum "${ARMS[$i]}/best_effort_throughput_p2/server" | cut -c1-16)"
done
SPIN=() CPIN=()
if [ -n "$PIN" ]; then
    read -r s c <<<"$PIN"
    SPIN=(taskset -c "$s") CPIN=(taskset -c "$c")
fi
EVENTS=cycles:u,instructions:u,cycles:k,instructions:k,task-clock
WIN=(--warmup-s 2 --cooldown-s 2)

run_one() { # arm_index cell rep
    local i=$1 cell=$2 rep=$3 name=${NAMES[$1]} arm=${ARMS[$1]} dir cargs=()
    case "$cell" in
    be_p1_max) dir=best_effort_throughput_p1 ;;
    be_p2_max) dir=best_effort_throughput_p2 ;;
    be_p2_paced) dir=best_effort_throughput_p2 cargs=(-i 0.000011) ;;
    rel_p2_max) dir=reliable_throughput_p2 ;;
    esac
    local d=$WORK/$cell.$name.$rep
    mkdir -p "$d"
    # The server's PID is written by the shell that then execs it, checked by /proc/PID/exe before the signal.
    # shellcheck disable=SC2016,SC2024
    sudo -n ip netns exec "$NS" perf stat -x, -o "$d/srv.perf" -e "$EVENTS" -- \
        sh -c 'echo $$ > "$0"; exec "$@"' "$d/srv.pid" "${SPIN[@]}" "$arm/$dir/server" -d $((DUR + 30)) "${WIN[@]}" \
        >"$d/srv.log" 2>&1 &
    local srv=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS" "${CPIN[@]}" "$arm/$dir/client" -d "$DUR" "${WIN[@]}" "${cargs[@]}" \
        >"$d/cli.log" 2>&1
    sleep 0.5
    local spid
    spid=$(cat "$d/srv.pid" 2>/dev/null)
    if [ -n "$spid" ] && [ "$(sudo -n ip netns exec "$NS" readlink "/proc/$spid/exe")" = "$(readlink -f "$arm/$dir/server")" ]; then
        sudo -n ip netns exec "$NS" kill -INT "$spid"
    else
        say "WARN server pid '$spid' is not $arm/$dir/server - left to its own cap"
    fi
    wait "$srv"
    {
        echo "RUN cell=$cell arm=$name rep=$rep"
        grep -h '^RESULT' "$d/cli.log" "$d/srv.log" | cut -c1-4000 || echo "NORESULT"
        grep -E 'cycles|instructions|task-clock' "$d/srv.perf" | sed 's/^/SRVPERF /' || echo "NOPERF"
    } >>"$OUT"
}

for rep in $(seq 1 "$REPS"); do
    for cell in $CELLS; do
        # Arm order rotates each rep (A A2 B, A2 B A, B A A2), so no arm always runs first or last.
        for k in 0 1 2; do
            run_one $(((k + rep) % 3)) "$cell" "$rep"
        done
    done
    say "rep $rep done $(date +%T) load='$(cut -d' ' -f1-3 /proc/loadavg)'"
done

python3 -I - "$OUT" <<'PYEOF'
import re, sys, statistics as st
path = sys.argv[1]
probe, runs, cur = {}, [], None
for l in open(path):
    l = l.rstrip('\n')
    if l.startswith('PROBE '):
        f = l.split()
        n = int(f[1].split('=')[1])
        vals = dict((x.split(',')[1].split(':')[0], float(x.split(',')[0])) for x in f[2:] if ',' in x)
        probe[n] = vals
    elif l.startswith('RUN '):
        cur = dict(kv.split('=', 1) for kv in l.split()[1:]); cur.update(cli=None, srv=None, perf={}); runs.append(cur)
    elif cur is None:
        continue
    elif l.startswith('RESULT:'):
        f = dict(kv.split('=', 1) for kv in l.split() if '=' in kv)
        cur['srv' if f.get('role') == 'server' else 'cli'] = f
    elif l.startswith('SRVPERF '):
        p = l.split(' ', 1)[1].split(',')
        try:
            cur['perf'][p[2]] = float(p[0])
        except (ValueError, IndexError):
            pass
ITERS = 10000000
def per_call(n, ev):
    return (probe[n][ev] - probe[0][ev]) / ITERS
print('--- summary ---')
for n in (110, 1320):
    print(f'memcpy probe {n} B: {per_call(n, "instructions"):.1f} instructions, {per_call(n, "cycles"):.1f} cycles per call (hot)')
DGRAM = {'be_p1_max': 110, 'be_p2_max': 1320, 'be_p2_paced': 1320, 'rel_p2_max': 1320}
cells = []
for r in runs:
    if r['cell'] not in cells:
        cells.append(r['cell'])
verdicts = {}
saving = {}
recv_ok = True
for cell in cells:
    rows = {}
    for r in runs:
        if r['cell'] != cell:
            continue
        c, s, p = r['cli'], r['srv'], r['perf']
        void = None
        if not c or not s: void = 'RESULT missing'
        elif c.get('window') != 'ok' or s.get('window') != 'ok': void = 'window not ok'
        elif 'cycles:u' not in p or 'instructions:u' not in p or 'task-clock' not in p: void = 'perf output missing'
        else:
            recv = int(s.get('recv', 0)); sent = int(c.get('sent', 0) or 0)
            if recv < 1000: void = f'recv={recv}'
            elif cell.startswith('be') and recv < 0.5 * sent: void = f'recv={recv} < 50% of sent={sent}'
        if void:
            print(f'VOID cell={cell} arm={r["arm"]} rep={r["rep"]}: {void}'); continue
        recv = int(s['recv'])
        calls = int(s.get('rx_batch_calls', 0)); dgr = int(s.get('rx_batch_datagrams', 0))
        row = dict(recv=recv, cyc_u=p['cycles:u'] / recv, ins_u=p['instructions:u'] / recv,
                   cyc_k=p.get('cycles:k', 0) / recv, task_us=p['task-clock'] * 1e3 / recv,
                   rows_share=(dgr - calls) / recv, per_batch=dgr / calls if calls else 0.0)
        rows.setdefault(r['arm'], []).append(row)
        print(f'cell={cell} arm={r["arm"]} rep={r["rep"]} ' + ' '.join(f'{k}={v:.4f}' if isinstance(v, float) else f'{k}={v}' for k, v in row.items()))
    n = {a: len(rows.get(a, [])) for a in ('A', 'A2', 'B')}
    if min(n.values()) < 4:
        verdicts[cell] = 'UNREADABLE'; print(f'CELL {cell} UNREADABLE n={n}'); continue
    med = {a: {k: st.median(x[k] for x in rows[a]) for k in rows[a][0]} for a in rows}
    for k in ('recv', 'cyc_u', 'ins_u', 'cyc_k', 'task_us', 'rows_share', 'per_batch'):
        print(f'MEDIAN cell={cell} {k}: A={med["A"][k]:.4f} A2={med["A2"][k]:.4f} B={med["B"][k]:.4f}  n={n}')
    out = {}
    for k in ('cyc_u', 'ins_u', 'task_us'):
        a = [x[k] for x in rows['A']]
        floor = max(abs(med['A2'][k] / med['A'][k] - 1), (max(a) - min(a)) / 2 / med['A'][k])
        d = med['B'][k] / med['A'][k] - 1
        out[k] = (d, floor)
        print(f'DELTA cell={cell} {k}: B-A {d*100:+.2f}%  A2-A {(med["A2"][k]/med["A"][k]-1)*100:+.2f}%  floor {floor*100:.2f}%')
    predicted = med['A']['rows_share'] * per_call(DGRAM[cell], 'instructions')
    dropped = med['A']['ins_u'] - med['B']['ins_u']
    mech = 0.5 * predicted <= dropped <= 2.0 * predicted if predicted > 0 else False
    print(f'MECHANISM cell={cell}: predicted ins_u drop {predicted:.1f}/sample (rows_share {med["A"]["rows_share"]:.3f} x '
          f'{per_call(DGRAM[cell], "instructions"):.1f}), measured {dropped:.1f} -> {"holds" if mech else "FAILS"}')
    d, floor = out['cyc_u']
    if d <= -(floor + 0.01):
        v = 'SAVES' if mech else 'NOT-THE-COPY'
    elif abs(d) <= floor + 0.01:
        v = 'SAME'
    else:
        v = 'WORSE'
    if med['B']['recv'] < 0.99 * med['A']['recv']:
        recv_ok = False
        print(f'RECV cell={cell}: B median recv {med["B"]["recv"]} < 99% of A {med["A"]["recv"]}')
    if v == 'SAVES' and d > -0.02:
        v = 'SAVES-UNDER-2%'
    verdicts[cell] = v
    saving[cell] =med['A']['cyc_u'] - med['B']['cyc_u']
    print(f'VERDICT cell={cell} {v} (cyc_u {d*100:+.2f}%, floor {floor*100:.2f}%)')
dose = None
if 'be_p1_max' in saving and 'be_p2_max' in saving and saving['be_p2_max'] > 0:
    dose = saving['be_p1_max'] / saving['be_p2_max']
    print(f'DOSE p1/p2 cycle saving ratio {dose:.2f} -> {"NOT-BYTES-PROPORTIONAL" if dose > 0.5 else "ok"}')
p2 = [verdicts.get(c) for c in ('be_p2_max', 'be_p2_paced')]
worth = (all(v == 'SAVES' for v in p2) and verdicts.get('rel_p2_max') != 'WORSE' and recv_ok
         and dose is not None and dose <= 0.5)
print('RIG_AB ' + ('WORTH RUNNING' if worth else 'NOT WORTH RUNNING') + f' verdicts={verdicts} dose={dose}')
PYEOF
rc=0
