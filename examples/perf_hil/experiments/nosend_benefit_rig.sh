#!/usr/bin/env bash
# nosend_benefit_rig.sh - does "a Publisher with no matched reader sends nothing" (faa178a3, ab/no-send-unmatched)
# improve anything, and what does it cost? Arm A is the control (the parent: a writer with no known reader broadcasts
# every sample to the link), arm B the change. Written 2026-10-15, before any run of it.
#
# The bench is nosend_bench.c (this directory), the same source for both arms, built against each arm's core (git
# archive of src/ and include/ at A and at B) on each host natively, twice: p1's 76-byte sample in the default build,
# and a 1 MiB sample (Array1m-like) in rmw's datagram build (-Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1). Both
# with -Dtt_SEGMENT_ENABLED=0, so the PC preflight's namespaces (which share /dev/shm) take the rig's wire path.
#
# Hosts: WRITER (Pi A, default 10.1.1.213) runs the writer; READER (Pi B, default 10.1.1.214) runs the observer and the
# late reader. READER is the Pi with passwordless `sudo -n tc` (setup-rpi-tc-sudoers.sh), so netem loss goes on ITS
# eth0 egress: what is lost is the reader's announce and its ACKNACKs - the traffic arm B's first sample waits on.
#
# CELL U ("unwatched topic"), U_DUR s (default 60) each, U_REPS (default 3) reps, arms interleaved A B / B A:
#   U0  the control: a BEST_EFFORT Publisher that publishes nothing (-r 0). The change cannot touch it; it measures
#       discovery and the link's background, which the other cells' data packets are taken net of.
#   Ua  p1 BEST_EFFORT 1 kHz     Ub  p1 RELIABLE 1 kHz     Uc  1 MiB RELIABLE 30 Hz
#   No Subscriber anywhere. READER runs an observer: a TickLE context of the same arm and build with no endpoint, on
#   the link from 2 s before the writer to after it. Recorded: the writer's RESULT (published, user+sys CPU over its
#   publishing window, per sample) and core's traffic line (the treatment: tx_unmatched_dropped/kept, B only); the
#   observer's RESULT (its CPU, rx_datagrams); each host's kernel counters before the writer starts and after it ends
#   (eth0 rx/tx packets and bytes, /proc/net/snmp Ip and Udp, /proc/stat cpu incl. softirq, /proc/softirqs NET_RX).
# CELL F ("time to first sample"), F_REPS (default 20) reps per arm per loss in F_LOSSES (default "0 5"), interleaved:
#   Fa  p1 BEST_EFFORT 1 kHz     Fb  p1 RELIABLE 1 kHz     Fc  1 MiB RELIABLE 30 Hz
#   A fresh writer starts on WRITER; F_LEAD s (default 5) later a fresh reader starts on READER and logs, on its own
#   monotonic clock, context creation -> first announce of another context (discovery: the control) and Subscriber
#   creation -> first sample (the measurement); it gives up after F_TIMEOUT s (default 10: "never").
#
# HOW TO READ IT - pre-registered in nosend_benefit_read.py, which decides; nothing is judged by hand:
#   U   PASS (B improved): in each of Ua Ub Uc, B's data packets (net of U0) on the writer's wire (eth0 tx) and at Pi B
#       (eth0 rx) are below 1% of A's, and the writer's CPU per sample is not WORSE (WORSE: B's median > 1.05 x A's
#       median AND B's lowest run above A's highest). VOID: any run without its RESULT lines, a binary that is not the
#       arm's build (/proc/PID/exe sha256 against the build's), a treatment not applied (B's writer must report every
#       publish as tx_unmatched_dropped/kept; A's must report none), A's writer not putting a data packet on the wire
#       for 95% of its samples (the premise), a control U0 that differs between the arms by more than max(50, 25%) packets.
#   F   per cell and loss: n, median, p90 and max of Subscriber -> first sample per arm; B - A median with a bootstrap
#       95% CI. REGRESSION: the median delay rises by more than 5 ms at 0% loss, or any B run never receives within
#       F_TIMEOUT. VOID: an A run that never receives (the setup, not the change), a missing RESULT, a wrong binary, a
#       B writer without tx_unmatched counts, or the discovery control's median moving by more than 5 ms at 0% loss.
#   Expected (written before the run): U - B ~0 data packets on the wire and at Pi B, A the full broadcast; F - B slower
#   by ~1-1.5 ms at 1 kHz (one announce and a flush tick), and at 5% loss ~5% of B's runs up to ~1 s.
#
# Usage (the lead runs the rig and wraps it in the lock; this script re-execs under rig_lock.sh if not already held):
#   A=<sha> B=<sha> setsid nohup examples/perf_hil/experiments/nosend_benefit_rig.sh \
#       > ~/rig_results_safe/nosend_benefit_<stamp>.log 2>&1 < /dev/null &
#   PC_PREFLIGHT=1 A=<sha> B=<sha> U_DUR=5 U_REPS=1 F_REPS=3 F_LEAD=3 nosend_benefit_rig.sh    (two private netns)
# Knobs: CELLS (default "U0 Ua Ub Uc Fa Fb Fc"), U_DUR, U_REPS, F_REPS, F_LOSSES, F_LEAD, F_TIMEOUT, WRITER, READER, OUT.
# Results: $OUT (default ~/rig_results_safe/nosend_benefit/<stamp>, PC: .../nosend_benefit_pc/<stamp>): reading.txt,
# index.txt, builds.txt, runs/. The last line is always "nosend benefit done rc=<n> <date>". Exit: 0 U PASS and F no
# regression, 1 refused or failed to run, 2 U FAIL or F REGRESSION, 3 VOID.
# Rig time at the defaults: builds ~10 min, U 24 runs x ~72 s ~29 min, F 240 runs x ~11 s ~44 min: ~85 min.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PC_PREFLIGHT=${PC_PREFLIGHT:-0}
export RIG_LOCK_SCOPE=hil
if [ "$PC_PREFLIGHT" != 1 ] && [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    exec "$REPO/examples/perf_hil/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi
MEM_KB=2097152
ulimit -v "$MEM_KB" # every PC process from here on; in PC_PREFLIGHT the benches too
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
CELLS=${CELLS:-"U0 Ua Ub Uc Fa Fb Fc"}
U_DUR=${U_DUR:-60}
U_REPS=${U_REPS:-3}
F_REPS=${F_REPS:-20}
F_LOSSES=${F_LOSSES:-"0 5"}
F_LEAD=${F_LEAD:-5}
F_TIMEOUT=${F_TIMEOUT:-10}
K=$HOME/.ssh/tickle_ci_ed25519
WRITER=${WRITER:-10.1.1.213}
READER=${READER:-10.1.1.214}
STAMP=$(date +%Y%m%d-%H%M%S)
rc=1
for v in U_DUR U_REPS F_REPS F_LEAD F_TIMEOUT; do
    case "${!v}" in '' | *[!0-9]*) echo "REFUSED: $v must be a whole number, not '${!v}'"; exit 1 ;; esac
done
for c in $CELLS; do case "$c" in U0 | Ua | Ub | Uc | Fa | Fb | Fc) ;; *) echo "REFUSED: unknown cell $c"; exit 1 ;; esac; done
for l in $F_LOSSES; do case "$l" in '' | *[!0-9]*) echo "REFUSED: F_LOSSES holds '$l'"; exit 1 ;; esac; done

A=$(git -C "$REPO" rev-parse --verify -q "${A:-origin/main}^{commit}") || { echo "REFUSED: no commit A"; exit 1; }
B=$(git -C "$REPO" rev-parse --verify -q "${B:?set B to the change}^{commit}") || { echo "REFUSED: no commit B=$B"; exit 1; }
[ "$A" != "$B" ] || { echo "REFUSED: A and B are the same commit"; exit 1; }
if [ "$PC_PREFLIGHT" != 1 ]; then
    for c in "$A" "$B"; do # a figure must name a commit others can fetch
        git -C "$REPO" branch -r --contains "$c" | grep -q . || { echo "REFUSED: ${c:0:8} is on no origin branch (push it)"; exit 1; }
    done
fi
HARNESS=$(git -C "$REPO" rev-parse HEAD)
HDIRTY=$(git -C "$REPO" status --porcelain -- examples/perf_hil/experiments/nosend_bench.c \
    examples/perf_hil/experiments/nosend_benefit_rig.sh examples/perf_hil/experiments/nosend_benefit_read.py | wc -l)
if [ "$PC_PREFLIGHT" = 1 ]; then
    OUT=${OUT:-$HOME/rig_results_safe/nosend_benefit_pc/$STAMP}
else
    OUT=${OUT:-$HOME/rig_results_safe/nosend_benefit/$STAMP}
    [ "$HDIRTY" = 0 ] || { echo "REFUSED: the harness files have uncommitted edits (a rig figure names its harness)"; exit 1; }
fi
[ -e "$OUT/index.txt" ] && { echo "REFUSED: $OUT already holds a run"; exit 1; }
mkdir -p "$OUT/runs" || exit 1
LOG=$OUT/run.log
say() { echo "$(date -Is) $*" | tee -a "$LOG"; }
python3 "$HERE/nosend_benefit_read.py" --selftest > "$OUT/selftest.txt" 2>&1 ||
    { tail -20 "$OUT/selftest.txt"; echo "REFUSED: nosend_benefit_read.py --selftest failed"; exit 1; }

# ---- hosts: two Pis over ssh, or two private namespaces on this PC (PC_PREFLIGHT=1) ----------------------------------
if [ "$PC_PREFLIGHT" = 1 ]; then
    # One preflight at a time: both "hosts" share this PC's file system and the per-role files under $WORK.
    PF_LOCK=/tmp/nosend_benefit_pc.$USER.lock
    exec 8>> "$PF_LOCK" || { echo "REFUSED: cannot open $PF_LOCK"; exit 75; }
    flock -n 8 || { echo "REFUSED: another nosend_benefit PC preflight holds $PF_LOCK"; exit 75; }
    WRITER=nsbw_$$
    READER=nsbr_$$
    WORK=${WORK:-$HOME/nosend_benefit_pc_work}
    rdir() { echo "$WORK/$1"; }
else
    rdir() { echo "/home/ci/nosend_benefit"; }
fi
sh_() { # host command...: run on that host (as this user in its namespace on the PC; under 2 GiB there too)
    local h=$1
    shift
    if [ "$PC_PREFLIGHT" = 1 ]; then
        sudo -n ip netns exec "$h" sudo -n -u "$USER" bash -c "ulimit -v $MEM_KB; $*"
    else
        ssh -n -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "$@"
    fi
}
sh_in() { # host command...: as sh_, with this shell's stdin passed through
    local h=$1
    shift
    if [ "$PC_PREFLIGHT" = 1 ]; then
        sudo -n ip netns exec "$h" sudo -n -u "$USER" bash -c "$*"
    else
        ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "$@"
    fi
}
pc_netns_up() {
    local ns peer ip
    sudo -n ip netns add "$WRITER" && sudo -n ip netns add "$READER" || return 1
    sudo -n ip link add "nb$$w" type veth peer name "nb$$r" || return 1
    for ns in "$WRITER" "$READER"; do
        case "$ns" in "$WRITER") peer=nb$$w ip=192.168.10.2 ;; *) peer=nb$$r ip=192.168.10.3 ;; esac
        sudo -n ip link set "$peer" netns "$ns" && sudo -n ip -n "$ns" link set "$peer" name eth0 &&
            sudo -n ip -n "$ns" addr add "$ip/24" brd 192.168.10.255 dev eth0 && sudo -n ip -n "$ns" link set lo up &&
            sudo -n ip -n "$ns" link set eth0 up && sudo -n ip -n "$ns" route add default dev eth0 || return 1
    done
}
pc_netns_down() {
    sudo -n ip netns del "$WRITER" 2> /dev/null
    sudo -n ip netns del "$READER" 2> /dev/null
    return 0
}
netem_on() { # does READER's eth0 carry netem now?
    if [ "$PC_PREFLIGHT" = 1 ]; then sudo -n ip netns exec "$READER" tc qdisc show dev eth0 2> /dev/null | grep -q netem
    else sh_ "$READER" "tc qdisc show dev eth0" 2> /dev/null | grep -q netem; fi
}
set_loss() { # percent: netem loss on READER's egress, read back
    if [ "$1" = 0 ]; then
        if [ "$PC_PREFLIGHT" = 1 ]; then sudo -n ip netns exec "$READER" tc qdisc del dev eth0 root > /dev/null 2>&1
        else sh_ "$READER" "sudo -n tc qdisc del dev eth0 root" > /dev/null 2>&1; fi
        ! netem_on
        return
    fi
    if [ "$PC_PREFLIGHT" = 1 ]; then sudo -n ip netns exec "$READER" tc qdisc replace dev eth0 root netem loss "$1%"
    else sh_ "$READER" "sudo -n tc qdisc replace dev eth0 root netem loss $1%"; fi
    local q
    if [ "$PC_PREFLIGHT" = 1 ]; then q=$(sudo -n ip netns exec "$READER" tc qdisc show dev eth0 2> /dev/null)
    else q=$(sh_ "$READER" "tc qdisc show dev eth0" 2> /dev/null); fi
    case "$q" in *"netem"*"loss $1%"*) return 0 ;; esac
    say "netem loss $1% not in place on $READER: $q"
    return 1
}
# stop host role: SIGINT the role's process if /proc/<pid>/exe is the binary it was launched as, wait 10 s, SIGKILL.
stop_role() {
    # shellcheck disable=SC2016 # \$ expanded on the host
    sh_ "$1" "d=$(rdir "$1"); p=\$(cat \$d/$2.pid 2>/dev/null) || exit 0; b=\$(cat \$d/$2.bin 2>/dev/null)
        [ -n \"\$b\" ] && [ \"\$(readlink /proc/\$p/exe 2>/dev/null)\" = \"\$b\" ] || exit 0
        kill -INT \$p; for _ in \$(seq 1 100); do [ \"\$(readlink /proc/\$p/exe 2>/dev/null)\" = \"\$b\" ] || exit 0; sleep 0.1; done
        kill -KILL \$p && echo \"$2 \$p ignored SIGINT for 10 s: SIGKILL\"" < /dev/null
}
# wait_role host role seconds: until the role's process has exited (by its own PID and exe), at most that long.
wait_role() {
    # shellcheck disable=SC2016 # \$ expanded on the host
    sh_ "$1" "d=$(rdir "$1"); p=\$(cat \$d/$2.pid 2>/dev/null) || exit 0; b=\$(cat \$d/$2.bin)
        for _ in \$(seq 1 $((10 * $3))); do [ \"\$(readlink /proc/\$p/exe 2>/dev/null)\" = \"\$b\" ] || exit 0; sleep 0.1; done; exit 1" < /dev/null
}
# launch host role binary args...: started detached; its PID, the binary's path and /proc/PID/exe's sha256 recorded.
launch() {
    local h=$1 role=$2 bin=$3
    shift 3
    local d
    d=$(rdir "$h")
    # shellcheck disable=SC2016 # \$ expanded on the host
    # The bench is backgrounded as a simple command of its own, so $! is its PID (nohup execs it): backgrounding the
    # whole "cd && rm && nohup" list put it in a subshell, whose PID $! was, and the first PC preflight's stops missed
    # every writer.
    sh_ "$h" "cd $d || exit 1; rm -f $role.pid $role.bin $role.exe $role.log
        nohup $d/$bin $* > $role.log 2>&1 < /dev/null &
        p=\$!; echo \$p > $d/$role.pid; echo $d/$bin > $d/$role.bin; sleep 0.3
        sha256sum /proc/\$p/exe 2>/dev/null | cut -c1-64 > $d/$role.exe" < /dev/null
}
# shellcheck disable=SC2329 # called from cleanup(), the EXIT trap
stop_all() {
    for r in pub obs sub; do stop_role "$WRITER" "$r" > /dev/null 2>&1; stop_role "$READER" "$r" > /dev/null 2>&1; done
    sweep
}
sweep() {
    # Any bench still running from this harness's directory, found by its /proc/PID/exe (the binary's own path,
    # never a command-line pattern), so a stop that missed one leaves nothing behind to load a later measurement.
    local h d
    for h in "$WRITER" "$READER"; do
        d=$(rdir "$h")
        # shellcheck disable=SC2016 # \$ expanded on the host
        sh_ "$h" "for e in /proc/[0-9]*/exe; do case \"\$(readlink \$e 2>/dev/null)\" in
            $d/*/nosend_small | $d/*/nosend_large) p=\${e#/proc/}; p=\${p%/exe}; kill -KILL \$p && echo \"left over: \$p\" ;; esac; done" < /dev/null
    done
}
# shellcheck disable=SC2329 # invoked by the EXIT trap below
cleanup() { # the one EXIT trap: remote processes, the link's shaping, the namespaces, the last line
    stop_all
    if ! set_loss 0; then echo "RIG LEFT SHAPED: netem still on $READER eth0 - clear it before any measurement" | tee -a "$LOG"; fi
    [ "$PC_PREFLIGHT" = 1 ] && pc_netns_down
    echo "nosend benefit done rc=$rc $(date -Is)" | tee -a "$LOG"
}
trap cleanup EXIT
trap 'rc=1; exit 1' TERM INT

if [ "$PC_PREFLIGHT" = 1 ]; then
    pc_netns_down
    pc_netns_up || { say "REFUSED: cannot build the namespaces"; exit 1; }
    mkdir -p "$(rdir "$WRITER")" "$(rdir "$READER")" || exit 1
elif netem_on; then
    # A run that died SIGKILLed left the rig shaped: refuse, rather than silently clearing what another job did.
    say "REFUSED: $READER eth0 already carries netem ($(sh_ "$READER" "tc qdisc show dev eth0" | head -1))"
    trap - EXIT
    echo "nosend benefit done rc=1 $(date -Is)"
    exit 1
fi
say "=== nosend benefit $(date -Is): A=${A:0:12} B=${B:0:12} harness=${HARNESS:0:12} (uncommitted harness files: $HDIRTY)" \
    "pc_preflight=$PC_PREFLIGHT writer=$WRITER reader=$READER cells=[$CELLS] U_DUR=$U_DUR U_REPS=$U_REPS F_REPS=$F_REPS" \
    "F_LOSSES=[$F_LOSSES] F_LEAD=$F_LEAD F_TIMEOUT=$F_TIMEOUT out=$OUT ==="
{
    echo "A=$A"
    echo "B=$B"
    echo "HARNESS=$HARNESS"
    echo "HARNESS_DIRTY=$HDIRTY"
    echo "PC_PREFLIGHT=$PC_PREFLIGHT"
    echo "CELLS=$CELLS"
    echo "U_DUR=$U_DUR"
    echo "U_REPS=$U_REPS"
    echo "F_REPS=$F_REPS"
    echo "F_LOSSES=$F_LOSSES"
    echo "F_LEAD=$F_LEAD"
    echo "F_TIMEOUT=$F_TIMEOUT"
} > "$OUT/meta.txt"

# ---- builds: each arm's core and this harness's bench, natively on each host ----------------------------------------
# kernel counters, one line of key=value: eth0, /proc/net/snmp Ip and Udp, /proc/stat's cpu line, /proc/softirqs.
# shellcheck disable=SC2016 # expanded on the host
KSNAP='#!/bin/bash
s=/sys/class/net/eth0/statistics
printf "t_ns=%s if_rx_packets=%s if_rx_bytes=%s if_tx_packets=%s if_tx_bytes=%s" "$(date +%s%N)" \
    "$(cat $s/rx_packets)" "$(cat $s/rx_bytes)" "$(cat $s/tx_packets)" "$(cat $s/tx_bytes)"
awk '"'"'/^(Ip|Udp):/ { k = $1; sub(":", "", k); n[k]++; for (i = 2; i <= NF; i++) { if (n[k] == 1) h[k, i] = $i; else printf " %s_%s=%s", k, h[k, i], $i } }'"'"' /proc/net/snmp
awk '"'"'NR == 1 { printf " cpu_user=%s cpu_nice=%s cpu_system=%s cpu_idle=%s cpu_iowait=%s cpu_irq=%s cpu_softirq=%s", $2, $3, $4, $5, $6, $7, $8 }'"'"' /proc/stat
awk '"'"'$1 == "NET_RX:" || $1 == "NET_TX:" { s = 0; for (i = 2; i <= NF; i++) s += $i; k = $1; sub(":", "", k); printf " softirqs_%s=%s", k, s }'"'"' /proc/softirqs
echo
'
build_host() { # host: every arm, both variants; one line per binary into $OUT/builds_<host>.txt
    local h=$1 d sha
    d=$(rdir "$h")
    sh_in "$h" "mkdir -p $d/harness && cat > $d/ksnap.sh && chmod +x $d/ksnap.sh" <<<"$KSNAP" || return 1
    sh_in "$h" "mkdir -p $d/harness && cat > $d/harness/nosend_bench.c" < "$HERE/nosend_bench.c" || return 1
    for sha in "$A" "$B"; do
        local t=${sha:0:12}
        git -C "$REPO" archive "$sha" src include | sh_in "$h" "rm -rf $d/$t && mkdir -p $d/$t && tar -x -C $d/$t" || return 1
        for v in small large; do
            local f="" bin=$t/nosend_$v
            [ "$v" = large ] && f="-Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1"
            # Built there, the old binary removed first; a failed build stops the run (a stale binary never stands in).
            sh_ "$h" "cd $d && rm -f $bin && timeout 900 gcc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 $f -DBENCH_TREE='\"$sha\"' \
                -I$t/include -I$t/src -o $bin harness/nosend_bench.c $t/src/tickle.c $t/src/encoding.c $t/src/log.c \
                $t/src/hal_linux.c -lpthread -lm > $t/build_$v.log 2>&1 && test -x $bin" < /dev/null ||
                { sh_ "$h" "tail -20 $d/$t/build_$v.log" < /dev/null; return 1; }
            echo "host=$h arm_sha=$sha variant=$v path=$d/$bin sha256=$(sh_ "$h" "sha256sum $d/$bin | cut -c1-64" < /dev/null)"
        done
    done
}
say "--- building A and B, small and large, on $WRITER and $READER (in parallel) ---"
build_host "$WRITER" > "$OUT/builds_w.txt" 2> "$OUT/builds_w.err" &
bw=$!
build_host "$READER" > "$OUT/builds_r.txt" 2> "$OUT/builds_r.err" &
br=$!
wait "$bw"
rw=$?
wait "$br"
rr=$?
cat "$OUT/builds_w.txt" "$OUT/builds_r.txt" > "$OUT/builds.txt"
if [ "$rw" != 0 ] || [ "$rr" != 0 ] || [ "$(grep -c 'sha256=[0-9a-f]\{64\}$' "$OUT/builds.txt")" != 8 ]; then
    cat "$OUT/builds_w.err" "$OUT/builds_r.err" | tail -20 | tee -a "$LOG"
    say "REFUSED: the builds did not give 8 binaries (writer rc $rw, reader rc $rr)"
    exit 1
fi
say "--- 8 binaries built ---"
sed 's/^/  /' "$OUT/builds.txt" | tee -a "$LOG" > /dev/null

ksnap() { sh_ "$1" "$(rdir "$1")/ksnap.sh" < /dev/null > "$2" 2>&1; }
fetch() { # host role stem: the role's log and the sha256 of its /proc/PID/exe
    local d
    d=$(rdir "$1")
    sh_ "$1" "cat $d/$2.log" < /dev/null > "$OUT/runs/${3}_$2.log" 2>&1
    sh_ "$1" "cat $d/$2.exe" < /dev/null > "$OUT/runs/${3}_$2.exe" 2>&1
}
arm_order() { # rep: A B on odd reps, B A on even
    if [ $(($1 % 2)) = 1 ]; then echo "A B"; else echo "B A"; fi
}
sha_of() { if [ "$1" = A ]; then echo "$A"; else echo "$B"; fi; }
want() { case " $CELLS " in *" $1 "*) return 0 ;; esac; return 1; }
cell_args() { # cell: variant and the writer's flags (and the reader's, for F)
    case "$1" in
    U0) echo "small -r 0" ;;
    Ua | Fa) echo "small -r 1000" ;;
    Ub | Fb) echo "small -R -r 1000" ;;
    Uc | Fc) echo "large -R -r 30" ;;
    esac
}

# ---- cell U --------------------------------------------------------------------------------------------------------
u_run() { # cell arm rep
    local cell=$1 arm=$2 rep=$3 sha t v wflags stem
    sha=$(sha_of "$arm")
    t=${sha:0:12}
    read -r v wflags <<<"$(cell_args "$cell")"
    stem=${cell}_${arm}_r$rep
    launch "$READER" obs "$t/nosend_$v" observe -d $((U_DUR + 12))
    sleep 2
    ksnap "$WRITER" "$OUT/runs/${stem}_wk_pre.txt"
    ksnap "$READER" "$OUT/runs/${stem}_rk_pre.txt"
    # shellcheck disable=SC2086 # the flags are deliberately word-split
    launch "$WRITER" pub "$t/nosend_$v" pub $wflags -d "$U_DUR"
    wait_role "$WRITER" pub $((U_DUR + 30)) || { say "  $stem: the writer did not end in $((U_DUR + 30)) s"; stop_role "$WRITER" pub; }
    ksnap "$WRITER" "$OUT/runs/${stem}_wk_post.txt"
    ksnap "$READER" "$OUT/runs/${stem}_rk_post.txt"
    stop_role "$READER" obs
    fetch "$WRITER" pub "$stem"
    fetch "$READER" obs "$stem"
    local lo
    lo=$(sweep | wc -l)
    [ "$lo" = 0 ] || say "  $stem: $lo bench process(es) left over, killed"
    echo "RUN kind=U cell=$cell arm=$arm sha=$sha rep=$rep loss=0 variant=$v stem=$stem writer=$WRITER reader=$READER leftover=$lo" >> "$OUT/index.txt"
    say "  $(date +%T) $stem: $(grep -m1 '^RESULT:' "$OUT/runs/${stem}_pub.log" | grep -oE 'published=[0-9]+ .*cpu_ns_per_sample=[0-9.]+' | cut -c1-120)"
}
for cell in U0 Ua Ub Uc; do
    want "$cell" || continue
    say "--- cell $cell: $(cell_args "$cell"), ${U_DUR}s, $U_REPS rep(s) per arm ---"
    for rep in $(seq 1 "$U_REPS"); do
        for arm in $(arm_order "$rep"); do u_run "$cell" "$arm" "$rep"; done
    done
done

# ---- cell F --------------------------------------------------------------------------------------------------------
f_run() { # cell arm rep loss
    local cell=$1 arm=$2 rep=$3 loss=$4 sha t v wflags rflags stem
    sha=$(sha_of "$arm")
    t=${sha:0:12}
    read -r v wflags <<<"$(cell_args "$cell")"
    rflags=""
    case "$wflags" in *-R*) rflags=-R ;; esac
    stem=${cell}_l${loss}_${arm}_r$rep
    # shellcheck disable=SC2086 # the flags are deliberately word-split
    launch "$WRITER" pub "$t/nosend_$v" pub $wflags -d 3600
    sleep "$F_LEAD"
    launch "$READER" sub "$t/nosend_$v" sub $rflags -t "$F_TIMEOUT" -l 0.5
    wait_role "$READER" sub $((F_TIMEOUT + 15)) || { say "  $stem: the reader did not end"; stop_role "$READER" sub; }
    stop_role "$WRITER" pub
    fetch "$WRITER" pub "$stem"
    fetch "$READER" sub "$stem"
    local lo
    lo=$(sweep | wc -l)
    [ "$lo" = 0 ] || say "  $stem: $lo bench process(es) left over, killed"
    echo "RUN kind=F cell=$cell arm=$arm sha=$sha rep=$rep loss=$loss variant=$v stem=$stem writer=$WRITER reader=$READER leftover=$lo" >> "$OUT/index.txt"
    say "  $(date +%T) $stem: $(grep -m1 '^RESULT:' "$OUT/runs/${stem}_sub.log" | grep -oE 'ctx_to_announce_ms=[-0-9.]+|sub_to_first_ms=[-0-9.]+' | tr '\n' ' ')"
    sleep 1
}
for loss in $F_LOSSES; do
    any=0
    for cell in Fa Fb Fc; do want "$cell" && any=1; done
    [ "$any" = 1 ] || break
    set_loss "$loss" || { say "tc failed for $loss%"; exit 1; }
    say "--- loss $loss% on $READER's egress ---"
    for cell in Fa Fb Fc; do
        want "$cell" || continue
        say "--- cell $cell: $(cell_args "$cell"), $F_REPS rep(s) per arm, loss $loss% ---"
        for rep in $(seq 1 "$F_REPS"); do
            for arm in $(arm_order "$rep"); do f_run "$cell" "$arm" "$rep" "$loss"; done
        done
    done
done
set_loss 0 || say "netem could not be cleared"
say "=== runs done; reading ==="

timeout 600 python3 "$HERE/nosend_benefit_read.py" "$OUT" > "$OUT/reading.txt" 2>&1
r=$?
tee -a "$LOG" < "$OUT/reading.txt"
case $r in
0) rc=0 ;;
2) rc=2 ;;
3) rc=3 ;;
*) say "the reader did not finish (rc $r)"; rc=3 ;;
esac
exit "$rc"
