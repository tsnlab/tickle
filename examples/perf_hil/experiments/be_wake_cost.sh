#!/usr/bin/env bash
# be_wake_cost.sh - what the same-host best_effort_throughput p3 pair costs per sample at max rate, on THIS PC, in a
# private network namespace. The max-rate counterpart of reader_wake_cost.sh (which measures one wake at a low rate):
# a change to the reader's wait must be read on both, because a cheaper wake can make the reader wake more often.
# A PC figure for steering a change, never for publication.
#
# Usage: be_wake_cost.sh ARM_DIR [ARM_DIR ...]
#   Each ARM_DIR holds a best_effort_throughput p3 client and server (examples/perf_hil/tickle/build.sh
#   best_effort_throughput p3), copied there so a rebuild cannot change an arm under the run.
# Env: REPS (5), DUR (5 s measured, plus 2 s warm-up and 2 s cool-down), MODE (time | trace | record),
#      PIN ("2 4": server and client CPUs; "" runs both unpinned), OUT (results file, outside any session directory).
# This PC is a KVM guest with no cpuidle driver: waking a halted vCPU costs a VM exit and an IPI that a Pi's core does
# not pay, so a reader here blocks for real far more often than on the rig, and the two can sit in different regimes.
#
# MODE=time: REPS interleaved runs per arm; per run both processes' user/sys time (perf stat, software counters only).
# MODE=trace: as time, plus every syscall of both processes counted by tracepoint. Perturbs timing; read the counts.
# MODE=record: one run per arm under perf record -g, files kept for perf report.
#
# READING RULES, enforced below:
#   VOID: a run whose client or server RESULT is missing, whose window is not ok, or whose server lost samples it
#     could not account for (recv < 99% of sent).
#   PER SAMPLE: user and sys time of both processes divided by the client's sent= (the whole run, as ab_samehost.py
#     derives utime/stime_per_sample_us). Doorbells (client doorbells_sent=) and syscalls are per sample too: a reader
#     that sleeps more often must show up as more rings per sample.
set -uo pipefail
REPS=${REPS:-5}
DUR=${DUR:-5}
MODE=${MODE:-time}
PIN=${PIN-2 4}
OUT=${OUT:-$HOME/wake_results/be_wake_cost_$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"
: >"$OUT"
[ $# -ge 1 ] || { echo "usage: $0 ARM_DIR [ARM_DIR ...]" >&2; exit 2; }
NS=tickle_bewake_$$
WORK=$(mktemp -d /tmp/be_wake_cost.XXXXXX)
cleanup() { sudo -n ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT
sudo -n ip netns add "$NS" || { echo "FATAL cannot create netns" | tee -a "$OUT"; exit 1; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo
sudo -n ip netns exec "$NS" ip link add bench0 type dummy
sudo -n ip netns exec "$NS" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip netns exec "$NS" ip link set bench0 up

say() { echo "$*" | tee -a "$OUT"; }
say "=== be_wake_cost $(date -Is) mode=$MODE reps=$REPS dur=$DUR pin='$PIN' ==="
for arm in "$@"; do
    say "arm $(basename "$arm") client $(sha256sum "$arm/client" | cut -c1-16) server $(sha256sum "$arm/server" | cut -c1-16)"
done
SYSCALLS="raw_syscalls:sys_enter,syscalls:sys_enter_ppoll,syscalls:sys_enter_epoll_pwait2,syscalls:sys_enter_read,syscalls:sys_enter_write,syscalls:sys_enter_io_uring_enter,syscalls:sys_enter_recvfrom,syscalls:sys_enter_recvmmsg,syscalls:sys_enter_futex"
EVENTS=context-switches,cpu-migrations,task-clock
reps=$REPS
case "$MODE" in
time) ;;
trace) EVENTS="context-switches,cpu-migrations,task-clock,$SYSCALLS" ;;
record) reps=1 ;;
*) echo "MODE must be time, trace or record" >&2; exit 2 ;;
esac
SPIN=()
CPIN=()
if [ -n "$PIN" ]; then
    read -r s c <<<"$PIN"
    SPIN=(taskset -c "$s")
    CPIN=(taskset -c "$c")
fi
WIN=(--warmup-s 2 --cooldown-s 2)

for rep in $(seq 1 "$reps"); do
    for arm in "$@"; do
        name=$(basename "$arm")
        d=$WORK/$name.$rep
        mkdir -p "$d"
        rm -f "$d/srv.pid"
        if [ "$MODE" = record ]; then
            SRVPERF=(perf record -q -g -c 20000 -e cycles -o "$d/srv.data")
            CLIPERF=(perf record -q -g -c 20000 -e cycles -o "$d/cli.data")
            : >"$d/srv.perf"; : >"$d/cli.perf"
        else
            SRVPERF=(perf stat -o "$d/srv.perf" -e "$EVENTS")
            CLIPERF=(perf stat -o "$d/cli.perf" -e "$EVENTS")
        fi
        # The server's PID is written by the shell that then execs it, checked by /proc/PID/exe before the signal.
        # shellcheck disable=SC2024,SC2016
        sudo -n ip netns exec "$NS" env BENCH_IFACE=lo "${SRVPERF[@]}" -- \
            sh -c 'echo $$ > "$0"; exec "$@"' "$d/srv.pid" "${SPIN[@]}" "$arm/server" -Q -d $((DUR + 30)) "${WIN[@]}" \
            >"$d/srv.log" 2>&1 &
        srv=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS" env BENCH_IFACE=lo "${CLIPERF[@]}" -- \
            "${CPIN[@]}" "$arm/client" -Q -d "$DUR" "${WIN[@]}" >"$d/cli.log" 2>&1
        sleep 0.5
        spid=$(cat "$d/srv.pid" 2>/dev/null)
        if [ -n "$spid" ] && [ "$(sudo -n ip netns exec "$NS" readlink "/proc/$spid/exe")" = "$(readlink -f "$arm/server")" ]; then
            sudo -n ip netns exec "$NS" kill -INT "$spid"
        else
            echo "WARN server pid '$spid' is not $arm/server - left to its own cap" | tee -a "$OUT"
        fi
        wait "$srv"
        if [ "$MODE" = record ]; then
            sudo -n ip netns exec "$NS" chmod a+r "$d/srv.data" "$d/cli.data"
            # shellcheck disable=SC2024
            [ -s "$WORK/kallsyms" ] || sudo -n ip netns exec "$NS" cat /proc/kallsyms >"$WORK/kallsyms"
        fi
        {
            echo "RUN arm=$name rep=$rep"
            grep -h '^RESULT' "$d/cli.log" "$d/srv.log" || echo "NORESULT"
            sed 's/^/CLIPERF /' "$d/cli.perf"
            sed 's/^/SRVPERF /' "$d/srv.perf"
        } >>"$OUT"
    done
done

python3 - "$OUT" "$MODE" "$@" <<'PYEOF'
import re, sys, statistics as st, os
path, mode, arms = sys.argv[1], sys.argv[2], [os.path.basename(a) for a in sys.argv[3:]]
runs, cur = [], None
for l in open(path):
    l = l.rstrip('\n')
    if l.startswith('RUN '):
        cur = dict(kv.split('=', 1) for kv in l.split()[1:]); cur.update(cli={}, srv={}, cperf={}, sperf={}); runs.append(cur)
    elif cur is None:
        continue
    elif l.startswith('RESULT:'):
        f = dict(kv.split('=', 1) for kv in l.split() if '=' in kv)
        cur['srv' if f.get('role') == 'server' else 'cli'] = f
    elif l.startswith(('CLIPERF ', 'SRVPERF ')):
        body = l.split(' ', 1)[1].strip()
        key = 'cperf' if l.startswith('CLI') else 'sperf'
        m = re.match(r'([\d,.]+)\s+seconds (user|sys)$', body)
        if m:
            cur[key][m.group(2)] = float(m.group(1).replace(',', '')); continue
        m = re.match(r'([\d,.]+) msec task-clock', body)
        if m:
            cur[key]['task-clock'] = float(m.group(1).replace(',', '')); continue
        m = re.match(r'([\d,]+)\s+(\S+)', body)
        if m and not body.startswith(('Performance', '#')):
            cur[key][m.group(2)] = float(m.group(1).replace(',', ''))
    elif l.startswith('NORESULT'):
        cur['void'] = 'no RESULT'
print(f'--- summary mode={mode} ---')
for arm in arms:
    rows = []
    for r in runs:
        if r['arm'] != arm: continue
        c, s = r['cli'], r['srv']
        void = r.get('void')
        if not void and (not c or not s): void = 'client or server RESULT missing'
        if not void and (c.get('window') != 'ok' or s.get('window') != 'ok'): void = 'window not ok'
        sent = int(c.get('sent', 0) or 0)
        if not void and int(s.get('recv', 0)) < 0.99 * sent: void = f"recv={s.get('recv')} < 99% of sent={sent}"
        if void:
            print(f'VOID arm={arm} rep={r["rep"]}: {void}'); continue
        cp, sp = r['cperf'], r['sperf']
        row = dict(rep=r['rep'], msps=sent / float(c['elapsed_s']) / 1e6,
                   usr_us=(cp.get('user', 0) + sp.get('user', 0)) * 1e6 / sent,
                   sys_us=(cp.get('sys', 0) + sp.get('sys', 0)) * 1e6 / sent,
                   cli_usr_us=cp.get('user', 0) * 1e6 / sent, cli_sys_us=cp.get('sys', 0) * 1e6 / sent,
                   srv_usr_us=sp.get('user', 0) * 1e6 / sent, srv_sys_us=sp.get('sys', 0) * 1e6 / sent,
                   rings_per_sample=int(c.get('doorbells_sent', 0)) / sent,
                   cs_per_sample=(cp.get('context-switches', 0) + sp.get('context-switches', 0)) / sent,
                   migr_per_ksample=(cp.get('cpu-migrations', 0) + sp.get('cpu-migrations', 0)) * 1e3 / sent,
                   head_stalls_per_sample=int(s.get('segment_head_stalls', 0)) / sent)
        if mode == 'trace':
            for k in sorted(set(cp) | set(sp)):
                if k.startswith(('syscalls:', 'raw_syscalls:')):
                    nm = k.split(':', 1)[1].replace('sys_enter_', '')
                    row['cli_' + nm] = cp.get(k, 0) / sent
                    row['srv_' + nm] = sp.get(k, 0) / sent
        rows.append(row)
        print(f'arm={arm} ' + ' '.join(f'{k}={v:.4f}' if isinstance(v, float) else f'{k}={v}' for k, v in row.items()))
    if rows:
        keys = [k for k in rows[0] if k != 'rep']
        print(f'MEDIAN arm={arm} n={len(rows)} ' + ' '.join(f'{k}={st.median(r[k] for r in rows):.4f}' for k in keys))
PYEOF
echo "results: $OUT  (per-run files: $WORK)"
