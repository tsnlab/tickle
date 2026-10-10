#!/usr/bin/env bash
# stepA_perf_rig.sh - where large-message step A (rebased) spends the extra time on the rig: perf stat, perf record and
# syscall counts of A = 2e9e861c and B = a5747a85 on the cells that read WORSE, interleaved A B B A. Written 2026-10-10
# before any of its runs.
#
# WHY. B vs A on the rig read client cpu_s_per_Msample +2.6% at p2 and +1.3-2.1% at p3 cross-host (campaign_ab_chain:
# largemsgL_l2x, placement_R, placement_P - and placement_P's -falign-functions=64 never reached the compiler: the chain
# ran /home/semih/tickle's campaign_sweep.sh, which does not forward TICKLE_EXTRA_CFLAGS, so P is a third plain
# replication), and same-host reliable_latency client rtt_p50 +3.6% at p2/p3 (ab_samehost largemsgL_l2s). On the PC
# (stepA_perf_pc.sh) user and kernel instructions and system calls per sample are equal A vs B on both paths, and the
# preprocessed core differs by nothing per sample in the default build (tt_LARGE_SAMPLES 0). This asks the Pi itself.
#
# CELLS (the campaign's and ab_samehost's own command lines, unpinned as they ran):
#   x_p2, x_p3  reliable_throughput -Q -N <405|368> -B 100 -d $DUR --warmup-s 2 --cooldown-s 2; server on 10.1.1.213,
#               client (the publisher, the role that read WORSE) on 10.1.1.214, over eth0.
#   s_p2        reliable_latency, both roles on 10.1.1.214, BENCH_IFACE=lo: server -Q -d $((DUR_S + 40)), client -Q -d
#               $DUR_S -i 0.005 -W 4096 -C 4096 -I 0.001 (s6_witness_check.sh's ON arm).
# MODES, each its own run of the same binary (an instrument perturbs timing, so only its counts are read from it):
#   plain   no instrument: the reproduction of the published gap (client cpu_s_per_Msample; s_p2 client rtt_p50_ms).
#   stat    perf stat -e instructions:u,instructions:k,cycles:u,cycles:k,context-switches,cpu-migrations,page-faults
#           [,raw_syscalls:sys_enter if the probe below finds tracepoints usable]; the client, and on s_p2 the server too.
#   record  perf record -g -e cycles -F $FREQ; the client, and on s_p2 the server too; reported per dso,symbol as cycles
#           (period) so a symbol's cycles per sample can be compared A vs B.
#   strace  strace -f -c: system calls per sample by name (only counts are read). Absent strace: "could not look".
# BLOCKS A B B A, REPS repetitions of every (cell, mode) per block, modes rotated inside the block.
#
# HOW TO READ IT (written before the first run; enforced by stepA_perf_read.py, whose header repeats it):
#   VOID run: a bench process left over from before it; no client RESULT line; sent = 0; x cells: drained != acked or tx_shm != 0 (not cross-host); s_p2: recv !=
#     sent or tx_shm/sent < 0.95 (the segment did not carry it); a mode's instrument output missing or empty.
#   TREATMENT: per variant, A's and B's client sha256 differ, or every comparison on that variant is VOID.
#   CONTROL: blocks 1 and 4 are both A. Per metric, FLOOR = |mean(A block 1) - mean(A block 4)| (the session's drift).
#   A metric DIFFERS when |B - A| > 2 x SE(B - A) AND > FLOOR AND >= its smallest effect (instructions 0.3%, cycles
#     0.5%, counts 0.01 per sample, a symbol 0.5% of its role's cycles). Per-sample metrics are divided by the client's
#     `sent` (round trips on s_p2). R1-R4 below count a metric only when B is the higher.
#   REPRODUCED: the plain mode's primary (x: client cpu_s_per_Msample; s_p2: client rtt_p50_ms) DIFFERS with B worse.
#     Not reproduced: everything below is printed but the reading says "no gap this session" and claims nothing.
#   Reading of a REPRODUCED cell, first match wins:
#     R1 instructions:u per sample DIFFERS                     -> more user work in B; the symbol deltas name it.
#     R2 syscalls per sample (any name, strace or tracepoint) DIFFERS -> a system call B adds or drops; named.
#     R3 instructions:k per sample DIFFERS, syscalls do not    -> more kernel work per call (what B asks the kernel).
#     R4 instructions u and k and syscalls equal, cycles:u or cycles:k DIFFERS -> the same work done slower: placement
#        or microarchitecture (i-cache, branch prediction, alignment), localised by the per-symbol cycle deltas; the
#        next step is a placement control that provably reaches the compiler (fixed campaign_ab_chain.sh, arm P).
#     R5 nothing above DIFFERS                                  -> the instruments cannot see the gap: could not look.
#   FALSIFIES "placement, not work" (d2798b64's reading): R1, R2 or R3 on any reproduced cell.
#   Per-symbol table: cycles per sample from record mode, A (blocks 1+4) vs B (2+3), the 25 largest |B - A|, each with
#     its own 2 x SE; a symbol present in one arm only is listed as such (code that moved or was split by gcc).
#
# ESTIMATE (perf_publisher_profile.sh and campaign timings): builds 2 arms x ~3 min; per block x cells 2 x 4 modes x
#   REPS 2 x ~17 s = ~4.5 min, s_p2 4 modes x 2 x ~32 s = ~4.3 min; 4 blocks = ~35 min. Total ~42 min; 1.5x = 63 min.
#
#   MODES="plain stat" (a subset, e.g. for a control arm whose question is only "does the primary still differ"):
#   builds as above, 4 blocks x ~5 min = ~27 min.
# Usage: A=<sha> B=<sha> [REPS=2] [DUR=5] [DUR_S=10] [FREQ=1999] [MODES=...] [OUT=prefix] stepA_perf_rig.sh
#   Takes the hil rig lock itself, once, for the whole run (rig_lock.sh). Launch detached; the deliverables are
#   $OUT.txt (log + reading) and $OUT.d/ (every run's raw files). The log always ends "=== stepA_perf ended rc=".
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X="$REPO/examples/perf_hil/experiments"
export RIG_LOCK_SCOPE=hil
export RIG_LOCK_WAIT="${RIG_LOCK_WAIT:-36000}"
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi

A=${A:?A=<full sha on origin>}
B=${B:?B=<full sha on origin>}
REPS=${REPS:-2}
DUR=${DUR:-5}
DUR_S=${DUR_S:-10}
FREQ=${FREQ:-1999}
OUT=${OUT:-$HOME/rig_results_safe/stepA_perf_$(date +%Y%m%d-%H%M%S)}
K=$HOME/.ssh/tickle_ci_ed25519
CLI=10.1.1.214
SRV=10.1.1.213
SAVE=/home/ci/stepA_perf # binaries per arm, outside ~/tickle so a later checkout cannot replace them mid-run
T0=$(date +%s)
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }

# The server's pid comes from its own launch (a pid file written by the launching shell before exec). Under perf stat
# or strace that pid is the instrument's, so the server itself is the instrument's child, read from /proc - and in
# every case nothing is signalled until its /proc/<pid>/exe is one of this run's server binaries.
SRV_HOST="" SRV_PIDFILE=/tmp/stepA_perf_srv.pid
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
    say "=== stepA_perf ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

say "=== stepA_perf $(date -Is): A=$A B=$B REPS=$REPS DUR=$DUR DUR_S=$DUR_S FREQ=$FREQ MODES='${MODES:-plain stat record strace}' harness=$(git -C "$REPO" rev-parse --short HEAD) ==="
say "    lock: $(cat /tmp/tickle-hil.lock 2>/dev/null)"

# ------------------------------------------------------------------------------------------------ probes
# What the Pis can count. paranoid 1 (2026-10-05) allows user and kernel counting of one's own processes; tracepoints
# also need tracefs, which may not be readable by ci - so it is tried, not assumed, and the answer is recorded.
TP_EV=""
STRACE_OK=1
for h in $CLI $SRV; do
    p=$(sh_ "$h" "echo host=\$(hostname) paranoid=\$(cat /proc/sys/kernel/perf_event_paranoid) perf=\$(perf --version 2>&1 | tr ' ' _) strace=\$(command -v strace || echo none)
perf stat -x, -e instructions:k true 2>&1 >/dev/null | grep -q '^[0-9][0-9]*,,instructions:k' && echo kcount=ok || echo kcount=FAIL
perf stat -x, -e raw_syscalls:sys_enter true 2>&1 >/dev/null | grep -q '^[0-9]' && echo tracepoint=ok || echo tracepoint=FAIL" </dev/null 2>&1 | tr '\n' ' ')
    say "PROBE $h: $p"
    case "$p" in *strace=none*) STRACE_OK=0 ;; esac
    case "$p" in *kcount=ok*) ;; *) say "FATAL: perf cannot count instructions:k on $h - the reading needs it"; exit 1 ;; esac
    if [ "$h" = $CLI ]; then case "$p" in *tracepoint=ok*) TP_EV=",raw_syscalls:sys_enter" ;; esac; fi
done
say "    syscalls by: ${TP_EV:+perf tracepoint raw_syscalls:sys_enter and }$([ $STRACE_OK = 1 ] && echo 'strace -c' || echo 'NO strace (could not look by name)')"
EVENTS="instructions:u,instructions:k,cycles:u,cycles:k,context-switches,cpu-migrations,page-faults$TP_EV"

# ------------------------------------------------------------------------------------------------ builds
# As campaign_sweep.sh builds: ~/tickle reset to the SHA, examples/perf_hil/tickle/build.sh <scen> <size>, default
# flags (TICKLE_EXTRA_CFLAGS empty), then the binaries copied to $SAVE/<arm>/<variant>/.
build_arm() { # $1 arm label, $2 sha, $3 host, $4 variants
    sh_ "$3" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $2 && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle
for v in $4; do
  scen=\${v%_p[0-9]}; size=\${v##*_}
  env -u TICKLE_EXTRA_CFLAGS ./build.sh \$scen \$size > /tmp/stepA_perf_build_\$v.log 2>&1 || { echo BUILD_FAILED \$v; tail -5 /tmp/stepA_perf_build_\$v.log; exit 1; }
  rm -rf $SAVE/$1/\$v && mkdir -p $SAVE/$1/\$v && cp \$v/client \$v/server $SAVE/$1/\$v/
  echo BUILT $1 \$v \$(git -C ~/tickle rev-parse --short HEAD) client=\$(sha256sum $SAVE/$1/\$v/client | cut -c1-16) server=\$(sha256sum $SAVE/$1/\$v/server | cut -c1-16) \$(grep -c -- '-O2' /tmp/stepA_perf_build_\$v.log | sed 's/^/O2_lines=/')
done" </dev/null 2>&1
}
for arm in A B; do
    sha=${!arm}
    build_arm $arm "$sha" $CLI "reliable_throughput_p2 reliable_throughput_p3 reliable_latency_p2" >"$OUT.d/build_${arm}_cli.txt" &
    pc=$!
    build_arm $arm "$sha" $SRV "reliable_throughput_p2 reliable_throughput_p3" >"$OUT.d/build_${arm}_srv.txt" &
    ps_=$!
    wait $pc
    wait $ps_
    sed "s/^/    /" "$OUT.d/build_${arm}_cli.txt" "$OUT.d/build_${arm}_srv.txt" | tee -a "$SUM"
    if grep -q BUILD_FAILED "$OUT.d/build_${arm}_cli.txt" "$OUT.d/build_${arm}_srv.txt"; then
        say "FATAL: a build of $arm failed (above)"
        exit 1
    fi
    n=$(cat "$OUT.d/build_${arm}_cli.txt" "$OUT.d/build_${arm}_srv.txt" | grep -c '^BUILT')
    [ "$n" = 5 ] || { say "FATAL: $n of 5 variants reported BUILT for $arm"; exit 1; }
done
say "    built in $(($(date +%s) - T0)) s"
# The builds' own identity lines are the treatment record; the reader re-checks A != B per variant from them.

# ------------------------------------------------------------------------------------------------ runs
cell_args() { # $1 cell -> the client's (and server's) arguments
    case "$1" in
    x_p2) echo "-Q -N 405 -B 100 -d $DUR --warmup-s 2 --cooldown-s 2" ;;
    x_p3) echo "-Q -N 368 -B 100 -d $DUR --warmup-s 2 --cooldown-s 2" ;;
    s_p2) echo "-Q -d $DUR_S -i 0.005 -W 4096 -C 4096 -I 0.001" ;;
    esac
}
prefix_for() { # $1 mode, $2 role -> the instrument in front of ./client or ./server on the Pi
    local f=/tmp/stepA_perf_$2
    case "$1" in
    plain) echo "" ;;
    stat) echo "perf stat -x, -o $f.perf -e $EVENTS --" ;;
    record) echo "perf record -q -g -e cycles -F $FREQ -o $f.data --" ;;
    strace) echo "strace -f -c -o $f.strace" ;;
    esac
}
# Fetches the instrument's output for one role into $2.<role>.<ext> (perf report rendered on the Pi, where the
# binaries and their symbols are).
fetch_instrument() { # $1 host, $2 local file prefix, $3 mode, $4 role
    local f=/tmp/stepA_perf_$4
    case "$3" in
    stat) sh_ "$1" "cat $f.perf" </dev/null >"$2.$4.perf" 2>&1 ;;
    strace) sh_ "$1" "cat $f.strace" </dev/null >"$2.$4.strace" 2>&1 ;;
    # period = the cycles attributed to the symbol. If this perf cannot print fields, the plain overhead table follows
    # under '#FALLBACK' and the reader converts shares with the stat mode's cycles instead.
    record) sh_ "$1" "r=\$(perf report -i $f.data --no-children -g none -q -t ';' -F period,sample,dso,sym --stdio 2>/dev/null)
if printf '%s\n' \"\$r\" | grep -q ';'; then printf '%s\n' \"\$r\"; else echo '#FALLBACK'; perf report -i $f.data --no-children -g none -q --sort dso,sym --stdio 2>&1 | head -200; fi
echo '# Samples:' \$(perf report -i $f.data --stdio 2>/dev/null | grep -m1 '^# Samples' | sed 's/.*Samples: *//')" </dev/null >"$2.$4.report" 2>&1 ;;
    esac
    [ "$3" = plain ] || sh_ "$1" "rm -f $f.perf $f.strace $f.data $f.data.old" </dev/null >/dev/null 2>&1
}
leftover() { # prints the exe of any bench process still alive on either Pi, by /proc/<pid>/exe
    for h in $CLI $SRV; do
        sh_ "$h" "for e in /proc/[0-9]*/exe; do x=\$(readlink \$e 2>/dev/null); case \"\$x\" in */perf_hil/*/client|*/perf_hil/*/server|$SAVE/*) echo \"$h:\$x\" ;; esac; done" </dev/null 2>/dev/null
    done | tr '\n' ' '
}
run_one() { # $1 block, $2 arm, $3 cell, $4 mode, $5 rep
    local blk=$1 arm=$2 cell=$3 mode=$4 rep=$5 args v shost sdur spre cpre f lo args_srv=""
    f="$OUT.d/b${blk}_${arm}_${cell}_${mode}_r${rep}"
    args=$(cell_args "$cell")
    case "$cell" in
    x_*) v=reliable_throughput_${cell#x_}; shost=$SRV; spre=""; sdur="" ;;
    s_*) v=reliable_latency_${cell#s_}; shost=$CLI; spre=$(prefix_for "$mode" server)
         args_srv="-Q -d $((DUR_S + 40))"; sdur=1 ;;
    esac
    [ "$mode" = strace ] && [ "$STRACE_OK" = 0 ] && { echo "SKIPPED no strace" >"$f.note"; return 0; }
    lo=$(leftover)
    [ -n "$lo" ] && echo "leftover before the run: $lo" >"$f.note"
    cpre=$(prefix_for "$mode" client)
    local env_s="" env_c="" sargs="$args"
    if [ -n "$sdur" ]; then env_s="env BENCH_IFACE=lo"; env_c="env BENCH_IFACE=lo"; sargs=$args_srv; fi
    SRV_HOST=$shost
    sh_ "$shost" "cd $SAVE/$arm/$v && rm -f $SRV_PIDFILE /tmp/stepA_perf_server.perf /tmp/stepA_perf_server.strace /tmp/stepA_perf_server.data
(setsid sh -c 'echo \$\$ > $SRV_PIDFILE; exec $env_s $spre ./server $sargs' > /tmp/stepA_perf_server.log 2>&1 < /dev/null &)
sleep 3; cat $SRV_PIDFILE" </dev/null >"$f.srvpid" 2>&1
    sh_ $CLI "cd $SAVE/$arm/$v && rm -f /tmp/stepA_perf_client.perf /tmp/stepA_perf_client.strace /tmp/stepA_perf_client.data
$env_c $cpre ./client $args > /tmp/stepA_perf_client.log 2>&1; echo client_rc=\$?; cat /tmp/stepA_perf_client.log" \
        </dev/null >"$f.client.log" 2>&1
    stop_server
    sleep 1
    sh_ "$shost" "cat /tmp/stepA_perf_server.log" </dev/null >"$f.server.log" 2>&1
    fetch_instrument $CLI "$f" "$mode" client
    [ -n "$sdur" ] && fetch_instrument "$shost" "$f" "$mode" server
    echo "$(date +%T) b$blk $arm $cell $mode r$rep $(grep -m1 '^RESULT' "$f.client.log" | tr ' ' '\n' | grep -E '^(sent|rtt_p50_ms|cpu_s_per_Msample|drained)=' | tr '\n' ' ')" >>"$SUM"
}
read -r -a MODES <<<"${MODES:-plain stat record strace}"
NM=${#MODES[@]}
blk=0
for arm in A B B A; do
    blk=$((blk + 1))
    say "--- block $blk arm $arm $(date -Is) ---"
    for rep in $(seq 1 "$REPS"); do
        for cell in x_p2 x_p3 s_p2; do
            for k in $(seq 0 $((NM - 1))); do
                run_one $blk $arm $cell "${MODES[$(((k + rep + blk) % NM))]}" "$rep"
            done
        done
    done
done
say "--- runs done at $(($(date +%s) - T0)) s ---"
python3 "$X/stepA_perf_read.py" "$OUT.d" | tee -a "$SUM"
