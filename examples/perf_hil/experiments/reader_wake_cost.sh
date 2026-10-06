#!/usr/bin/env bash
# reader_wake_cost.sh - what one same-host reader wake costs, on THIS PC, in a private network namespace
# (ROADMAP "Now" 4: ~3.1 us of system time per wake). A PC figure for steering a change, never for publication.
#
# Usage: reader_wake_cost.sh ARM_DIR [ARM_DIR ...]
#   Each ARM_DIR holds a reliable_latency p3 client and server (examples/perf_hil/tickle/build.sh reliable_latency p3),
#   copied there so a rebuild of the tree cannot change an arm under the run. The arm's name is the directory's.
# Env: REPS (5), DUR (5 s measured), SPACING (0.005 s between a reply and the next ping), MODE (time | trace),
#      OUT (results file, outside any session directory).
#
# MODE=time: REPS interleaved runs per arm, each process under `perf stat -e context-switches` (a software counter,
#   no per-syscall cost). Per run: rtt_p50, round trips, sleeps, and both processes' user/sys time from rusage.
# MODE=trace: one run per arm with every syscall of both processes counted by tracepoint
#   (raw_syscalls + the named ones below). MODE=count: kernel and user instructions, REPS runs. Both perturb timing, so its RTT is not read; it answers "how many syscalls
#   of which kind per wake", which a timing run cannot.
#
# READING RULES, enforced below:
#   VOID: a run whose client RESULT is missing, whose window is not ok, or whose measured round trips are under 80%
#     of DUR/SPACING (a run that idled instead of pinging would otherwise report a cheap wake).
#   PER WAKE: sys time and syscalls are divided by the reader sleeps both sides report (sleeps=), not by round trips:
#     a round trip that costs two sleeps on one side must show up as cost, not hide in the divisor. Both divisors
#     are printed.
#   The control the change cannot touch: user time per round trip. The wake path is system time; if user time
#     moves as much as system time between arms, the difference is the machine, not the change.
set -uo pipefail
REPS=${REPS:-5}
DUR=${DUR:-5}
SPACING=${SPACING:-0.005}
MODE=${MODE:-time}
OUT=${OUT:-$HOME/wake_results/reader_wake_cost_$(date +%Y%m%d-%H%M%S).txt}
mkdir -p "$(dirname "$OUT")"
: >"$OUT"
[ $# -ge 1 ] || { echo "usage: $0 ARM_DIR [ARM_DIR ...]" >&2; exit 2; }
NS=tickle_wake_$$
WORK=$(mktemp -d /tmp/reader_wake_cost.XXXXXX)
cleanup() { sudo -n ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT
sudo -n ip netns add "$NS" || { echo "FATAL cannot create netns" | tee -a "$OUT"; exit 1; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo
# The bench's compiled-in link is the rig's broadcast address; a dummy link carries it inside the namespace.
sudo -n ip netns exec "$NS" ip link add bench0 type dummy
sudo -n ip netns exec "$NS" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip netns exec "$NS" ip link set bench0 up

say() { echo "$*" | tee -a "$OUT"; }
say "=== reader_wake_cost $(date -Is) mode=$MODE reps=$REPS dur=$DUR spacing=$SPACING ==="
for arm in "$@"; do
    say "arm $(basename "$arm") client $(sha256sum "$arm/client" | cut -c1-16) server $(sha256sum "$arm/server" | cut -c1-16)"
done

SYSCALLS="raw_syscalls:sys_enter,syscalls:sys_enter_ppoll,syscalls:sys_enter_read,syscalls:sys_enter_write,syscalls:sys_enter_io_uring_enter,syscalls:sys_enter_recvfrom,syscalls:sys_enter_recvmmsg,syscalls:sys_enter_sendto,syscalls:sys_enter_futex,syscalls:sys_enter_clock_nanosleep"
# Software counters only in MODE=time: per-task hardware counters are saved and restored on every context switch,
# and on this PC that alone moved the p50 from 30 to 81 us. MODE=count takes the instruction counts, which that
# perturbation does not change, and MODE=trace the syscalls; neither run's timing is read.
EVENTS=context-switches,task-clock
reps=$REPS
case "$MODE" in
time) ;;
count) EVENTS="context-switches,task-clock,instructions:u,instructions:k" ;;
trace) EVENTS="context-switches,task-clock,$SYSCALLS" ;;
record) reps=1 ;;
summary) reps=1 ;; # perf trace -s: every syscall by name, count and time, into $WORK/<arm>.1/{cli,srv}.trace # perf record -g of both processes into $WORK/<arm>.1/{cli,srv}.data, kept for perf report
*) echo "MODE must be time, count, trace, summary or record" >&2; exit 2 ;;
esac

PINGS=$(python3 -c "print(int($DUR/$SPACING))")
for rep in $(seq 1 "$reps"); do
    for arm in "$@"; do
        name=$(basename "$arm")
        d=$WORK/$name.$rep
        mkdir -p "$d"
        # The client ends itself after warm-up + DUR + cool-down, every round trip at SPACING (-I as well as -i).
        # The server would idle on for its safety cap, adding sleeps nobody rang, so it is ended with SIGINT the
        # moment the client is done. Its PID is written by the shell that then execs it, and checked by
        # /proc/PID/exe before the signal - never found by a pattern.
        rm -f "$d/srv.pid"
        if [ "$MODE" = summary ]; then
            SRVPERF=(perf trace -s -o "$d/srv.trace")
            CLIPERF=(perf trace -s -o "$d/cli.trace")
            : >"$d/srv.perf"; : >"$d/cli.perf"
        elif [ "$MODE" = record ]; then
            SRVPERF=(perf record -q -g -c 20000 -e cycles -o "$d/srv.data")
            CLIPERF=(perf record -q -g -c 20000 -e cycles -o "$d/cli.data")
            : >"$d/srv.perf"; : >"$d/cli.perf"
        else
            SRVPERF=(perf stat -o "$d/srv.perf" -e "$EVENTS")
            CLIPERF=(perf stat -o "$d/cli.perf" -e "$EVENTS")
        fi
        # SC2024: the redirects are meant to stay ours - the logs belong to this user, not to root. SC2016: $$ is the
        # inner shell's.
        # shellcheck disable=SC2024,SC2016
        sudo -n ip netns exec "$NS" env BENCH_IFACE=lo "${SRVPERF[@]}" -- \
            sh -c 'echo $$ > "$0"; exec taskset -c 2 "$1" -W 256 -C 256 -I "$2" -d "$3"' \
            "$d/srv.pid" "$arm/server" "$SPACING" "$DUR" >"$d/srv.log" 2>&1 &
        srv=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS" env BENCH_IFACE=lo "${CLIPERF[@]}" -- \
            taskset -c 4 "$arm/client" -W 256 -C 256 -I "$SPACING" -i "$SPACING" -d "$DUR" >"$d/cli.log" 2>&1
        spid=$(cat "$d/srv.pid" 2>/dev/null)
        if [ -n "$spid" ] && [ "$(sudo -n ip netns exec "$NS" readlink "/proc/$spid/exe")" = "$(readlink -f "$arm/server")" ]; then
            sudo -n ip netns exec "$NS" kill -INT "$spid"
        else
            echo "WARN server pid '$spid' is not $arm/server - left to its own cap" | tee -a "$OUT"
        fi
        wait "$srv"
        if [ "$MODE" = summary ]; then
            sudo -n ip netns exec "$NS" chmod a+r "$d/srv.trace" "$d/cli.trace"
        fi
        if [ "$MODE" = record ]; then
            sudo -n ip netns exec "$NS" chmod a+r "$d/srv.data" "$d/cli.data"
            # Kernel addresses are hidden from this user; perf report --kallsyms=$WORK/kallsyms resolves them.
            # shellcheck disable=SC2024 # the copy is meant to be this user's
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

python3 - "$OUT" "$PINGS" "$MODE" "$@" <<'PYEOF'
import re, sys, statistics as st, os
path, pings, mode, arms = sys.argv[1], int(sys.argv[2]), sys.argv[3], [os.path.basename(a) for a in sys.argv[4:]]
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
def per(a, b): return a / b if b else float('nan')
print(f'--- summary mode={mode} pings_expected={pings} ---')
for arm in arms:
    rows = []
    for r in runs:
        if r['arm'] != arm: continue
        c, s = r['cli'], r['srv']
        void = r.get('void')
        if not void and c.get('window') != 'ok': void = 'window=' + c.get('window', '?')
        if not void and int(c.get('measured', 0)) < 0.8 * pings: void = f"measured={c.get('measured')} < 80% of {pings}"
        if void:
            print(f'VOID arm={arm} rep={r["rep"]}: {void}'); continue
        rt = int(c['measured']) + int(c.get('warmup', 0)) + int(c.get('cooldown', 0))
        sleeps = int(c['sleeps']) + int(s.get('sleeps', 0))
        row = dict(rep=r['rep'], p50=float(c['rtt_p50_ms']) * 1e3, rt=rt, sleeps=sleeps,
                   sleeps_per_rt=sleeps / rt,
                   sys_us_per_rt=(r['cperf'].get('sys', 0) + r['sperf'].get('sys', 0)) * 1e6 / rt,
                   sys_us_per_sleep=(r['cperf'].get('sys', 0) + r['sperf'].get('sys', 0)) * 1e6 / sleeps,
                   usr_us_per_rt=(r['cperf'].get('user', 0) + r['sperf'].get('user', 0)) * 1e6 / rt,
                   cpu_us_per_rt=(r['cperf'].get('task-clock', 0) + r['sperf'].get('task-clock', 0)) * 1e3 / rt,
                   kins_per_sleep=(r['cperf'].get('instructions:k', 0) + r['sperf'].get('instructions:k', 0)) / sleeps,
                   uins_per_rt=(r['cperf'].get('instructions:u', 0) + r['sperf'].get('instructions:u', 0)) / rt,
                   cs_per_rt=(r['cperf'].get('context-switches', 0) + r['sperf'].get('context-switches', 0)) / rt)
        if mode == 'trace':
            for k in sorted(set(r['cperf']) | set(r['sperf'])):
                if k.startswith(('syscalls:', 'raw_syscalls:')):
                    row[k.split(':', 1)[1].replace('sys_enter_', '') + '_per_sleep'] = \
                        per(r['cperf'].get(k, 0) + r['sperf'].get(k, 0), sleeps)
        rows.append(row)
        print(f'arm={arm} ' + ' '.join(f'{k}={v:.3f}' if isinstance(v, float) else f'{k}={v}' for k, v in row.items()))
    if rows:
        keys = [k for k in rows[0] if k not in ('rep',)]
        print(f'MEDIAN arm={arm} n={len(rows)} ' + ' '.join(f'{k}={st.median(r[k] for r in rows):.3f}' for k in keys))
PYEOF
echo "results: $OUT  (per-run files: $WORK)"
