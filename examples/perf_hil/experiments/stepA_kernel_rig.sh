#!/usr/bin/env bash
# stepA_kernel_rig.sh - is step A's extra kernel work per sample on the Pi work B asks the kernel for, or interrupt work
# charged to a publisher that is on-CPU longer? A = 2e9e861c vs B = a5747a85, interleaved A B B A. Written 2026-10-11
# before any of its runs.
#
# WHY. stepA_perf_rig.sh (443bff61, run 1 2026-10-11 00:15) read x_p2 client instructions:k +270 per sample (+1.97%,
# 2SE 157, n4) with system calls (strace -c: 1.0266/sample, sendto 1.005), datagrams sent and received per sample
# (traffic line: 1.00497 / 0.00497), wire bytes per sample (1354.607), context switches and page faults all equal, and
# no clock_gettime system call in either arm (the vDSO serves tt_get_ns in both). What B asks the kernel is therefore
# the same call for call. Two things still move kernel instructions per sample without a different request:
#   (i)  hard-irq and softirq work - eth0 TX completion, NET_TX qdisc restarts, NET_RX of ACKNACKs, timer ticks - runs
#        on whichever task is on-CPU, and perf counts it as that task's instructions:k. B's client runs cycles:u +4.6%
#        at equal instructions:u (the R4 gap), so it is on-CPU longer and absorbs more of it.
#   (ii) the in-call path depends on timing: a sendto() that finds the qdisc empty and the ring free transmits through
#        the driver in its own context; one that finds the ring full only enqueues.
# Run 2 (B = a27b5f76) read x_p2 instructions:k +194 (2SE 204: same), x_p3 read same in both runs, and s_p2 client
# read +780 (2SE 738) in run 2 only: the effect is near the noise at n4, so this harness also asks whether it is real.
#
# ARMS. x_p2 (reliable_throughput -Q -N 405 -B 100, client = publisher on 10.1.1.214, server on 10.1.1.213, eth0) in
#   three placements of the client process (perf and the client both under taskset; the server is never pinned):
#     free  unpinned, as every published run (the replication).
#     off   pinned to OFF_CPU, the client Pi's CPU that takes the fewest eth0 interrupts (probe below): (i) is removed,
#           (ii) is not.
#     on    pinned to IRQ_CPU, the CPU that takes the most eth0 interrupts: (i) at its maximum. A's on vs A's off is the
#           POSITIVE CONTROL - if charging interrupt work to the task cannot be seen there, (i) cannot be decided here.
#   s_p2 (reliable_latency same-host over the segment, both roles on 10.1.1.214) free only: does run 2's client
#   instructions:k +780 reproduce, and if so do IPIs (rescheduling, function call) per round trip differ - the wake path.
#   Pinning is a diagnostic here, never a published figure (headline figures stay unpinned).
# MEASURED per run, mode perf stat only (instructions u/k, cycles u/k, context-switches, cpu-migrations, page-faults),
#   plus, around the client's run on the client Pi, system-wide before/after snapshots (tracepoints FAILED on the Pis
#   2026-10-11, so counters that need no privilege): /proc/interrupts (eth0 lines per CPU, IPI lines), /proc/softirqs,
#   /proc/stat cpu lines, /proc/net/snmp Udp, /proc/net/softnet_stat, tc -s qdisc show dev eth0.
#   Treatment record per run: the client's cpu_main / cpu_main_share (from the bench's own getcpu samples) and the
#   eth0 interrupts that landed on the pinned CPU during the run.
#
# HOW TO READ IT (written before the first run; enforced by stepA_kernel_read.py, whose header repeats it):
#   VOID run: a bench process left over; no client RESULT; sent = 0; x: drained != acked or tx_shm != 0; s: recv != sent
#     or tx_shm/sent < 0.95; perf stat missing or not counted; snapshot pre or post missing; a pinned run whose client
#     did not stay on its CPU (cpu_main != pin or cpu_main_share < 0.99); an off run with >= 5% of the eth0 interrupts
#     on its CPU, an on run with < 90% (the IRQ moved).
#   TREATMENT: per variant A's and B's client sha256 differ, else the cell is VOID.
#   DIFFERS per (cell, placement, metric): |B - A| > 2 SE and > FLOOR (|A block 1 - A block 4|) and >= min effect
#     (instructions 0.3%, cycles 0.5%, a kernel event count 1% of A's mean and >= 0.002 per sample). A = blocks 1+4,
#     B = blocks 2+3.
#   x_p2, with D_p = B - A client instructions:k per sample in placement p:
#     K0 D_free does not DIFFER with B higher  -> run 1's +270 does not reproduce at n6: no kernel finding; the step A
#        gap is cycles at equal work (R4). (off and on are printed, and claim nothing.)
#     K1 D_free DIFFERS (B higher) AND D_off does not DIFFER AND D_off < D_free / 2 AND the positive control passes
#        (A on - A off instructions:k > 2 SE and >= 1% of A off) -> interrupt work charged to a publisher that is on-CPU
#        longer: B asks the kernel for nothing more. Also printed: eth0 interrupts, NET_TX and NET_RX softirqs per
#        sample system-wide A vs B, which should then be equal (the same interrupt work, attributed differently).
#     K2 D_off DIFFERS (B higher) AND D_off >= D_free / 2 -> more kernel work inside B's own calls; the system-wide
#        counters that DIFFER (qdisc requeues, NET_TX, Udp, softnet) name it, or "per-call, unnamed by these counters".
#     K? anything else (incl. K1's conditions without the positive control) -> printed as inconclusive with the values.
#   s_p2: S0 client instructions:k does not DIFFER -> run 2's +780 was noise at n4. S1 it DIFFERS (B higher) and an IPI
#     or softirq count per round trip DIFFERS (B higher) -> the wake path took a different branch (timing), named.
#     S2 it DIFFERS and none does -> unnamed.
#   Every reading also prints cycles:u per placement: whether the R4 gap survives pinning (information, not a verdict).
#
# ESTIMATE: builds ~1-4 min; per block x_p2 3 placements x REPS 3 x ~18 s = ~2.7 min, s_p2 3 x ~31 s = ~1.6 min; four
#   blocks ~17 min. Total ~22 min; 1.5x = 33 min.
# Usage: A=<sha> B=<sha> [REPS=3] [DUR=5] [DUR_S=10] [OUT=prefix] stepA_kernel_rig.sh
#   Takes the hil rig lock itself, once, for the whole run (rig_lock.sh). Launch detached; the deliverables are
#   $OUT.txt (log + reading) and $OUT.d/ (raw files). The log always ends "=== stepA_kernel ended rc=".
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X="$REPO/examples/perf_hil/experiments"
export RIG_LOCK_SCOPE=hil
export RIG_LOCK_WAIT="${RIG_LOCK_WAIT:-36000}"
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi

A=${A:?A=<full sha on origin>}
B=${B:?B=<full sha on origin>}
REPS=${REPS:-3}
DUR=${DUR:-5}
DUR_S=${DUR_S:-10}
OUT=${OUT:-$HOME/rig_results_safe/stepA_kernel_$(date +%Y%m%d-%H%M%S)}
K=$HOME/.ssh/tickle_ci_ed25519
CLI=10.1.1.214
SRV=10.1.1.213
SAVE=/home/ci/stepA_kernel # binaries per arm, outside ~/tickle so a later checkout cannot replace them mid-run
SNAP=/tmp/stepA_kernel_snap.sh
T0=$(date +%s)
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }

# The server's pid comes from its own launch (a pid file written by the launching shell before exec); under perf stat
# that pid is the instrument's, so the server is its child, read from /proc - and nothing is signalled until its
# /proc/<pid>/exe is one of this run's server binaries.
SRV_HOST="" SRV_PIDFILE=/tmp/stepA_kernel_srv.pid
stop_server() {
    [ -n "$SRV_HOST" ] || return 0
    sh_ "$SRV_HOST" "p=\$(cat $SRV_PIDFILE 2>/dev/null) || exit 0
is_srv() { case \"\$(readlink /proc/\$1/exe 2>/dev/null)\" in $SAVE/*/server) return 0 ;; esac; return 1; }
t=\$p
if ! is_srv \$t; then for c in \$(cat /proc/\$p/task/*/children 2>/dev/null); do is_srv \$c && t=\$c; done; fi
if is_srv \$t; then kill -INT \$t; for i in 1 2 3 4 5 6 7 8 9 10; do is_srv \$t || break; sleep 0.5; done; is_srv \$t && kill -KILL \$t; fi
for i in 1 2 3 4 5 6; do [ -d /proc/\$p ] || break; sleep 0.5; done
rm -f $SRV_PIDFILE; true" </dev/null >/dev/null 2>&1
    SRV_HOST=""
}
on_exit() {
    local rc=$?
    stop_server
    say "=== stepA_kernel ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

say "=== stepA_kernel $(date -Is): A=$A B=$B REPS=$REPS DUR=$DUR DUR_S=$DUR_S harness=$(git -C "$REPO" rev-parse --short HEAD) ==="
say "    lock: $(cat /tmp/tickle-hil.lock 2>/dev/null)"

# ------------------------------------------------------------------------------------------------ probes
# The snapshot script, installed on the client Pi. Everything it reads is world-readable; tc -s needs no privilege.
sh_ $CLI "cat > $SNAP && chmod +x $SNAP" <<'EOF'
#!/bin/sh
echo "#T $(date +%s.%N)"
echo "#INTERRUPTS"; cat /proc/interrupts
echo "#SOFTIRQS"; cat /proc/softirqs
echo "#STAT"; grep '^cpu' /proc/stat
echo "#SNMP"; grep '^Udp:' /proc/net/snmp
echo "#SOFTNET"; cat /proc/net/softnet_stat
echo "#QDISC"; tc -s qdisc show dev eth0 2>&1
echo "#END"
EOF
probe=$(sh_ $CLI "echo host=\$(hostname) ncpu=\$(nproc) paranoid=\$(cat /proc/sys/kernel/perf_event_paranoid) taskset=\$(command -v taskset || echo none) tc=\$(command -v tc || echo none)
perf stat -x, -e instructions:k true 2>&1 >/dev/null | grep -q '^[0-9][0-9]*,,instructions:k' && echo kcount=ok || echo kcount=FAIL
echo irq_time_acct=\$( (zcat /proc/config.gz 2>/dev/null || cat /boot/config-\$(uname -r) 2>/dev/null) | grep -E '^CONFIG_IRQ_TIME_ACCOUNTING=' || echo unknown)
for l in \$(grep -n eth0 /proc/interrupts | cut -d: -f2 | tr -d ' '); do echo irq\$l=aff:\$(cat /proc/irq/\$l/smp_affinity_list 2>/dev/null); done
echo rps=\$(cat /sys/class/net/eth0/queues/rx-0/rps_cpus 2>/dev/null) xps=\$(cat /sys/class/net/eth0/queues/tx-0/xps_cpus 2>/dev/null)
$SNAP | grep -c '^#END' | sed 's/^/snap_end=/'" </dev/null 2>&1 | tr '\n' ' ')
say "PROBE $CLI: $probe"
case "$probe" in *kcount=ok*) ;; *) say "FATAL: perf cannot count instructions:k on $CLI"; exit 1 ;; esac
case "$probe" in *snap_end=1*) ;; *) say "FATAL: the snapshot script does not run on $CLI"; exit 1 ;; esac
# IRQ_CPU / OFF_CPU from the eth0 interrupt counts per CPU so far (cumulative since boot: where the IRQ is routed).
read -r IRQ_CPU OFF_CPU < <(sh_ $CLI "grep eth0 /proc/interrupts" </dev/null 2>/dev/null | python3 -c '
import sys
n = None; tot = None
for line in sys.stdin:
    p = line.split()[1:]
    c = []
    for t in p:
        if not t.isdigit():
            break
        c.append(int(t))
    tot = c if tot is None else [a + b for a, b in zip(tot, c)]
if not tot:
    print("none none"); sys.exit()
on = max(range(len(tot)), key=lambda i: tot[i])
off = min((i for i in range(len(tot)) if i != on), key=lambda i: (tot[i], -i))
print(on, off)')
PLACEMENTS="free off on"
if [ "${IRQ_CPU:-none}" = none ] || [[ "$probe" == *taskset=none* ]]; then
    say "    no eth0 line in /proc/interrupts or no taskset: the off/on placements cannot run - free only (K1/K2 could not look)"
    PLACEMENTS="free"
    IRQ_CPU=none OFF_CPU=none
fi
say "    IRQ_CPU=$IRQ_CPU OFF_CPU=$OFF_CPU placements: $PLACEMENTS"
echo "IRQ_CPU=$IRQ_CPU OFF_CPU=$OFF_CPU" >"$OUT.d/placement.txt"
EVENTS="instructions:u,instructions:k,cycles:u,cycles:k,context-switches,cpu-migrations,page-faults"

# ------------------------------------------------------------------------------------------------ builds
build_arm() { # $1 arm label, $2 sha, $3 host, $4 variants
    sh_ "$3" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $2 && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle
for v in $4; do
  scen=\${v%_p[0-9]}; size=\${v##*_}
  env -u TICKLE_EXTRA_CFLAGS ./build.sh \$scen \$size > /tmp/stepA_kernel_build_\$v.log 2>&1 || { echo BUILD_FAILED \$v; tail -5 /tmp/stepA_kernel_build_\$v.log; exit 1; }
  rm -rf $SAVE/$1/\$v && mkdir -p $SAVE/$1/\$v && cp \$v/client \$v/server $SAVE/$1/\$v/
  echo BUILT $1 \$v \$(git -C ~/tickle rev-parse --short HEAD) client=\$(sha256sum $SAVE/$1/\$v/client | cut -c1-16) server=\$(sha256sum $SAVE/$1/\$v/server | cut -c1-16)
done" </dev/null 2>&1
}
for arm in A B; do
    sha=${!arm}
    build_arm $arm "$sha" $CLI "reliable_throughput_p2 reliable_latency_p2" >"$OUT.d/build_${arm}_cli.txt" &
    pc=$!
    build_arm $arm "$sha" $SRV "reliable_throughput_p2" >"$OUT.d/build_${arm}_srv.txt" &
    ps_=$!
    wait $pc
    wait $ps_
    sed "s/^/    /" "$OUT.d/build_${arm}_cli.txt" "$OUT.d/build_${arm}_srv.txt" | tee -a "$SUM"
    if grep -q BUILD_FAILED "$OUT.d/build_${arm}_cli.txt" "$OUT.d/build_${arm}_srv.txt"; then
        say "FATAL: a build of $arm failed (above)"
        exit 1
    fi
    n=$(cat "$OUT.d/build_${arm}_cli.txt" "$OUT.d/build_${arm}_srv.txt" | grep -c '^BUILT')
    [ "$n" = 3 ] || { say "FATAL: $n of 3 variants reported BUILT for $arm"; exit 1; }
done
say "    built in $(($(date +%s) - T0)) s"

# ------------------------------------------------------------------------------------------------ runs
leftover() { # the exe of any bench process still alive on either Pi, by /proc/<pid>/exe
    for h in $CLI $SRV; do
        sh_ "$h" "for e in /proc/[0-9]*/exe; do x=\$(readlink \$e 2>/dev/null); case \"\$x\" in */perf_hil/*/client|*/perf_hil/*/server|/home/ci/stepA_*) echo \"$h:\$x\" ;; esac; done" </dev/null 2>/dev/null
    done | tr '\n' ' '
}
run_one() { # $1 block, $2 arm, $3 cell, $4 placement, $5 rep
    local blk=$1 arm=$2 cell=$3 pl=$4 rep=$5 f v shost args sargs env_s="" env_c="" spre="" pin=""
    f="$OUT.d/b${blk}_${arm}_${cell}_${pl}_r${rep}"
    case "$pl" in off) pin="taskset -c $OFF_CPU" ;; on) pin="taskset -c $IRQ_CPU" ;; esac
    case "$cell" in
    x_p2) v=reliable_throughput_p2; shost=$SRV; args="-Q -N 405 -B 100 -d $DUR --warmup-s 2 --cooldown-s 2"; sargs=$args ;;
    s_p2) v=reliable_latency_p2; shost=$CLI; args="-Q -d $DUR_S -i 0.005 -W 4096 -C 4096 -I 0.001"
          sargs="-Q -d $((DUR_S + 40))"; env_s="env BENCH_IFACE=lo"; env_c="env BENCH_IFACE=lo"
          spre="perf stat -x, -o /tmp/stepA_kernel_server.perf -e $EVENTS --" ;;
    esac
    local lo
    lo=$(leftover)
    [ -n "$lo" ] && echo "leftover before the run: $lo" >"$f.note"
    SRV_HOST=$shost
    sh_ "$shost" "cd $SAVE/$arm/$v && rm -f $SRV_PIDFILE /tmp/stepA_kernel_server.perf
(setsid sh -c 'echo \$\$ > $SRV_PIDFILE; exec $env_s $spre ./server $sargs' > /tmp/stepA_kernel_server.log 2>&1 < /dev/null &)
sleep 3; cat $SRV_PIDFILE" </dev/null >"$f.srvpid" 2>&1
    sh_ $CLI "cd $SAVE/$arm/$v && rm -f /tmp/stepA_kernel_client.perf /tmp/stepA_kernel_pre.txt /tmp/stepA_kernel_post.txt
$SNAP > /tmp/stepA_kernel_pre.txt 2>&1
$env_c $pin perf stat -x, -o /tmp/stepA_kernel_client.perf -e $EVENTS -- ./client $args > /tmp/stepA_kernel_client.log 2>&1; echo client_rc=\$?
$SNAP > /tmp/stepA_kernel_post.txt 2>&1
cat /tmp/stepA_kernel_client.log" </dev/null >"$f.client.log" 2>&1
    stop_server
    sleep 1
    sh_ "$shost" "cat /tmp/stepA_kernel_server.log" </dev/null >"$f.server.log" 2>&1
    sh_ $CLI "cat /tmp/stepA_kernel_client.perf" </dev/null >"$f.client.perf" 2>&1
    sh_ $CLI "cat /tmp/stepA_kernel_pre.txt" </dev/null >"$f.pre.txt" 2>&1
    sh_ $CLI "cat /tmp/stepA_kernel_post.txt" </dev/null >"$f.post.txt" 2>&1
    [ "$cell" = s_p2 ] && sh_ "$shost" "cat /tmp/stepA_kernel_server.perf" </dev/null >"$f.server.perf" 2>&1
    echo "$(date +%T) b$blk $arm $cell $pl r$rep $(grep -m1 '^RESULT' "$f.client.log" | tr ' ' '\n' | grep -E '^(sent|rtt_p50_ms|cpu_s_per_Msample|drained|cpu_main|cpu_main_share)=' | tr '\n' ' ')" >>"$SUM"
}
read -r -a PL <<<"$PLACEMENTS"
NP=${#PL[@]}
blk=0
for arm in A B B A; do
    blk=$((blk + 1))
    say "--- block $blk arm $arm $(date -Is) ---"
    for rep in $(seq 1 "$REPS"); do
        for k in $(seq 0 $((NP - 1))); do
            run_one $blk $arm x_p2 "${PL[$(((k + rep + blk) % NP))]}" "$rep"
        done
        run_one $blk $arm s_p2 free "$rep"
    done
done
say "--- runs done at $(($(date +%s) - T0)) s ---"
python3 "$X/stepA_kernel_read.py" "$OUT.d" | tee -a "$SUM"
