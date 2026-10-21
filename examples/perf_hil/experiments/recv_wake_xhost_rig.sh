#!/usr/bin/env bash
# Receive wake cost Pi -> Pi over eth0, by wait style: is "wait and read in one io_uring call" worth building?
#
# Why (2026-10-21): the rig's wake split (docs/RESULTS.md, KH, 715a01f7) has rmw_tickle's pong receive returning ~4-5
# us after the vendors' on BEST_EFFORT (p50 18.0 against 12.8 / 13.8 us). Its wait (epoll_pwait2) returns as early as
# their blocking recvmsg; the gap is the second system call, the recvfrom() after the wait. The 4(b) analysis proposed
# one io_uring call that waits and reads (IORING_OP_RECV, with DEFER_TASKRUN, or multishot with provided buffers) and
# said: measure it on the Pi first, drop the idea if the Pi gains less than 0.5 us over the epoll style.
#
# What runs: recv_wake_xhost.c (its header says what is measured and how), built on both Pis from the commit SHA. The
# receiver is the KH pong Pi (10.1.1.213), the sender the other (10.1.1.214), over eth0, one sender and one receiver.
# Arms (ARMS): recvfrom recvfrom_aa recvmsg epoll3 uring uring_dt uring_ms uring_ms_dt prod prod_dt - recvfrom_aa is
# recvfrom under a second name, the A/A control; prod is production's own receive and prod_dt the same with its wait and
# read replaced by one DEFER_TASKRUN io_uring call (added 2026-10-22; decided against prod, "PROD VERDICT"). Every rep runs every arm once, in an order rotated by one per rep. ROUNDS (20100)
# datagrams a run after WARMUP (1000), so a run keeps >= 20,000 samples if a few are lost; REPS 5.
# The reading rules are code in recv_wake_xhost_read.py (--prereg prints them, --selftest checks them), written before
# any run; this script runs it at the end. The decision: an io_uring style gains >= 0.5 us at p50 over epoll3 with
# non-overlapping rep ranges -> worth implementing; else drop; the A/A control failing -> no verdict; a compared rep
# range wider than 1.0 us with no gain -> no verdict (NOISY: re-run), not drop. The same rule decides prod_dt against
# prod (PROD VERDICT), separately.
#
# Guards, enforced here:
#   - the rig lock (rig_lock.sh) taken ONCE, by this process, for every run;
#   - SHA must be on origin/main (fetched here); the Pis compile `git show SHA:<this .c>`, and its md5 must equal the
#     PC's of the same blob, or nothing runs;
#   - refuses to start if either Pi's eth0 carries a netem or tbf qdisc, and stops if one appears between reps (this
#     script sets none);
#   - one EXIT trap: stops the receiver it launched (by the PID the launcher wrote, only if /proc/PID/exe is the binary
#     built here), deletes the PC namespaces, prints RWX_END (always, success or not);
#   - per run: the receiver's /proc/PID/exe is the binary built here, its RXRESULT names the arm's style, the sender
#     printed TXRESULT; otherwise VOID. A style the kernel refuses is REFUSED (not measured), not VOID.
#
# USAGE (detached, from a durable checkout at a pushed SHA):
#   TS=$(date +%Y%m%d-%H%M%S); OUT=$HOME/rig_results_safe/recv_wake_xhost_<sha8>_$TS
#   setsid nohup env SHA=<sha> OUT=$OUT examples/perf_hil/experiments/recv_wake_xhost_rig.sh > $OUT.launch.log 2>&1 \
#     < /dev/null &
#   Wait on "RWX_END" in $OUT.txt (printed on every exit); "RWX_DONE" precedes it when the run completed.
# DURATION (rig): build ~1 min; a run is 21,100 datagrams x ~0.36 ms (200 us gap + sleep overshoot + a ~90 us round
#   trip) = ~7.6 s plus ~4 s of ssh = ~12 s; 10 arms x 5 reps = 50 runs = ~10 min; total ~12 min, analyse an overrun
#   at 18 min. (Plus the wait for the lock: RIG_LOCK_WAIT, default 1800 s.)
#
# PC_PREFLIGHT=1 (no rig, no lock, no sha check): "sender" and "receiver" are two private network namespaces joined by
# a veth named eth0 in each (192.168.10.3 and .2); every host command runs in its namespace as this user; the binary is
# compiled here (nice) from `git show SHA:...` (SHA defaults to HEAD). The PC's numbers say the harness works, not what
# the Pi does.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
SRC_PATH=examples/perf_hil/experiments/recv_wake_xhost.c
PC_PREFLIGHT=${PC_PREFLIGHT:-0}
export RIG_LOCK_SCOPE=hil
# shellcheck source=examples/perf_hil/experiments/pi_checkout.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime
. "$X/pi_checkout.sh"

K=$HOME/.ssh/tickle_ci_ed25519; SENDER=10.1.1.214; RECEIVER=10.1.1.213
REPS=${REPS:-5}
ROUNDS=${ROUNDS:-20100}
WARMUP=${WARMUP:-1000}
GAP_US=${GAP_US:-200}
RX_CPU=${RX_CPU:-2}
TX_CPU=${TX_CPU:-}
PORT=${PORT:-47611}
ECHO_PORT=${ECHO_PORT:-47612}
ARMS=${ARMS:-"recvfrom recvfrom_aa recvmsg epoll3 uring uring_dt uring_ms uring_ms_dt prod prod_dt"}
case "$REPS$ROUNDS$WARMUP$GAP_US$RX_CPU${TX_CPU:-0}$PORT$ECHO_PORT" in
    *[!0-9]*) echo "REPS ROUNDS WARMUP GAP_US RX_CPU TX_CPU PORT ECHO_PORT are whole numbers" >&2; exit 64 ;;
esac
for a in $ARMS; do
    case "$a" in recvfrom | recvfrom_aa | recvmsg | epoll3 | uring | uring_dt | uring_ms | uring_ms_dt | prod | prod_dt) ;;
        *) echo "unknown arm '$a'" >&2; exit 64 ;; esac
done
[ "$ROUNDS" -ge 20000 ] || echo "note: ROUNDS $ROUNDS < 20000 - every run will read VOID (n < 20000)" >&2

if [ "$PC_PREFLIGHT" = 1 ]; then
    SENDER=rwxs_$$; RECEIVER=rwxr_$$
    SHA=$(git -C "$REPO" rev-parse --verify "${SHA:-HEAD}^{commit}") || { echo "REFUSED: unknown SHA" >&2; exit 64; }
    TX_CPU=${TX_CPU:-4} # both namespaces share this PC: keep the sender off the receiver's CPU
else
    SHA=${SHA:?set SHA to a commit on origin/main}
    git -C "$REPO" fetch -q origin main || { echo "REFUSED: cannot fetch origin main" >&2; exit 1; }
    SHA=$(git -C "$REPO" rev-parse --verify --quiet "$SHA^{commit}") || { echo "REFUSED: unknown SHA" >&2; exit 1; }
    git -C "$REPO" merge-base --is-ancestor "$SHA" origin/main ||
        { echo "REFUSED: $SHA is not on origin/main" >&2; exit 1; }
    TX_CPU=${TX_CPU:-2} # the sender's own Pi: the core recv_wake_cost_rpi.sh found idle (the NIC IRQ is on core 0)
fi
git -C "$REPO" cat-file -e "$SHA:$SRC_PATH" 2>/dev/null || { echo "REFUSED: $SRC_PATH is not in $SHA" >&2; exit 1; }
SRC_MD5=$(git -C "$REPO" show "$SHA:$SRC_PATH" | md5sum | cut -d' ' -f1) || { echo "REFUSED: git show" >&2; exit 1; }
# The lock is taken after the cheap refusals above (a wrong SHA does not wait 30 min for the rig to say so), and the
# whole script runs again inside it, those checks included.
if [ "$PC_PREFLIGHT" != 1 ] && [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
OUT=${OUT:-$HOME/rig_results_safe/recv_wake_xhost_${SHA:0:8}_$(date +%Y%m%d-%H%M%S)}
R=$OUT.runs
mkdir -p "$R" || exit 1
SUM=$OUT.txt
say() { echo "$*" | tee -a "$SUM"; }
RD=/tmp/rwx_${SHA:0:8}_$$ # the run directory on each host (on the PC both namespaces share it)
BIN=$RD/rwx

sh_() {
    local h=$1; shift
    if [ "$PC_PREFLIGHT" = 1 ]; then
        sudo -n ip netns exec "$h" sudo -n -u "$USER" bash -c "$*"
    else
        ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "$@"
    fi
}
fetch_() { # host remote local
    if [ "$PC_PREFLIGHT" = 1 ]; then cp "$2" "$3" 2>/dev/null; else scp -q -i "$K" -o BatchMode=yes "ci@$1:$2" "$3" 2>/dev/null; fi
}
qdisc_of() { # host: eth0's qdiscs
    if [ "$PC_PREFLIGHT" = 1 ]; then sudo -n ip netns exec "$1" tc qdisc show dev eth0; else sh_ "$1" "tc qdisc show dev eth0" </dev/null; fi
}
link_unshaped() { # refuse a netem/tbf on either eth0; prints the qdisc lines
    local h q
    for h in "$SENDER" "$RECEIVER"; do
        q=$(qdisc_of "$h") || { say "REFUSED: cannot read the qdisc of $h"; return 1; }
        case "$q" in *netem* | *tbf*) say "REFUSED: $h eth0 is shaped ($q)"; return 1 ;; esac
        echo "qdisc $h: $(echo "$q" | head -1)" >>"$SUM"
    done
}

# rx_alive / rx_stop: the receiver this script launched, by the PID its launcher wrote, only if /proc/PID/exe is BIN.
rx_alive() {
    # shellcheck disable=SC2016 # expanded on the host
    sh_ "$RECEIVER" 'p=$(cat '"$RD"'/rx.pid 2>/dev/null); [ -n "$p" ] && [ "$(readlink /proc/$p/exe 2>/dev/null)" = '"$BIN"' ]' </dev/null
}
rx_stop() {
    # shellcheck disable=SC2016 # expanded on the host
    sh_ "$RECEIVER" 'p=$(cat '"$RD"'/rx.pid 2>/dev/null); [ -n "$p" ] && [ "$(readlink /proc/$p/exe 2>/dev/null)" = '"$BIN"' ] && kill "$p"' \
        </dev/null >/dev/null 2>&1
}

PC_VETH=rwx$$
pc_netns_up() {
    local ns peer ip
    sudo -n ip netns add "$SENDER" && sudo -n ip netns add "$RECEIVER" || return 1
    sudo -n ip link add "${PC_VETH}s" type veth peer name "${PC_VETH}r" || return 1
    for ns in "$SENDER" "$RECEIVER"; do
        if [ "$ns" = "$SENDER" ]; then peer=${PC_VETH}s ip=192.168.10.3; else peer=${PC_VETH}r ip=192.168.10.2; fi
        sudo -n ip link set "$peer" netns "$ns" && sudo -n ip -n "$ns" link set "$peer" name eth0 &&
            sudo -n ip -n "$ns" addr add "$ip/24" brd 192.168.10.255 dev eth0 && sudo -n ip -n "$ns" link set lo up &&
            sudo -n ip -n "$ns" link set eth0 up && sudo -n ip -n "$ns" route add default dev eth0 || return 1
    done
}

cleanup() { # the one EXIT trap
    rx_stop
    if [ "$PC_PREFLIGHT" = 1 ]; then
        sudo -n ip netns del "$SENDER" 2>/dev/null
        sudo -n ip netns del "$RECEIVER" 2>/dev/null
        rm -rf "$RD"
    else
        for h in "$SENDER" "$RECEIVER"; do sh_ "$h" "rm -rf $RD" </dev/null >/dev/null 2>&1; done
    fi
    echo "RWX_END $(date -Is) out=$OUT" >>"$SUM"
}
trap cleanup EXIT
if [ "$PC_PREFLIGHT" = 1 ]; then
    pc_netns_up || { say "PC preflight: cannot build the namespaces"; exit 1; }
    # PF_NETEM (PC only): a netem left on the receiver's eth0 - the shaped-link refusal must stop the run.
    case "${PF_NETEM:-}" in *[!A-Za-z0-9.\ %]*) say "PF_NETEM may hold only [A-Za-z0-9. %]"; exit 64 ;; esac
    if [ -n "${PF_NETEM:-}" ]; then
        # shellcheck disable=SC2086 # netem's words, checked above
        sudo -n ip netns exec "$RECEIVER" tc qdisc add dev eth0 root netem $PF_NETEM || exit 1
    fi
elif [ -n "${PF_NETEM:-}" ]; then
    say "PF_NETEM is for PC_PREFLIGHT=1 only"; exit 64
fi

say "=== recv_wake_xhost, $(date -Is): SHA $SHA (source md5 $SRC_MD5), $REPS reps x $ROUNDS rounds (+$WARMUP warm-up)," \
    "gap $GAP_US us, rx cpu $RX_CPU, tx cpu $TX_CPU, arms: $ARMS, pc preflight $PC_PREFLIGHT" \
    "(sender $SENDER, receiver $RECEIVER), out $OUT ==="
python3 "$X/recv_wake_xhost_read.py" --prereg >"$R/prereg.txt" || { say "REFUSED: the reader does not load"; exit 1; }
python3 "$X/recv_wake_xhost_read.py" --selftest >"$R/selftest.txt" 2>&1 ||
    { say "REFUSED: the reader's selftest fails ($R/selftest.txt)"; exit 1; }
say "reader selftest: $(tail -1 "$R/selftest.txt")"
link_unshaped || exit 1

# Build on each host from the commit's blob; the md5 of what was compiled must be the PC's.
SRC_TEXT=$(git -C "$REPO" show "$SHA:$SRC_PATH")
build_on() {
    local h=$1 fetch_text=""
    [ "$PC_PREFLIGHT" = 1 ] || fetch_text="cd ~/tickle && $(pi_fetch "$SHA") && "
    if [ "$PC_PREFLIGHT" = 1 ]; then
        mkdir -p "$RD" && printf '%s\n' "$SRC_TEXT" >"$RD/rwx.c" && nice -n 10 cc -O2 -o "$BIN" "$RD/rwx.c" &&
            echo "built on PC, md5 $(md5sum <"$RD/rwx.c" | cut -d' ' -f1)"
    else
        sh_ "$h" "set -e; ${fetch_text}mkdir -p $RD && git -C ~/tickle show $SHA:$SRC_PATH > $RD/rwx.c &&
cc -O2 -o $BIN $RD/rwx.c && echo \"built on \$(hostname) (\$(uname -r)), md5 \$(md5sum < $RD/rwx.c | cut -d' ' -f1)\"" </dev/null
    fi
}
bad=0
for h in "$SENDER" "$RECEIVER"; do
    line=$(build_on "$h" 2>&1) || bad=1
    say "$line"
    case "$line" in *"md5 $SRC_MD5"*) ;; *) bad=1; say "BUILD on $h did not compile md5 $SRC_MD5" ;; esac
    [ "$PC_PREFLIGHT" = 1 ] && break # one build serves both namespaces
done
[ "$bad" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }
RX_IP=$(sh_ "$RECEIVER" "ip -4 -o addr show dev eth0" </dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
TX_IP=$(sh_ "$SENDER" "ip -4 -o addr show dev eth0" </dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
case "$RX_IP$TX_IP" in *[!0-9.]* | "") say "REFUSED: no eth0 address (receiver '$RX_IP', sender '$TX_IP')"; exit 1 ;; esac
say "eth0: receiver $RX_IP, sender $TX_IP"
sh_ "$RECEIVER" "$BIN probe" </dev/null 2>&1 | tee "$R/probe.txt" | sed 's/^/probe: /' | tee -a "$SUM"
printf 'sha=%s\nsrc_md5=%s\nreps=%s\nrounds=%s\nwarmup=%s\ngap_us=%s\nrx_cpu=%s\ntx_cpu=%s\narms=%s\npc_preflight=%s\n' \
    "$SHA" "$SRC_MD5" "$REPS" "$ROUNDS" "$WARMUP" "$GAP_US" "$RX_CPU" "$TX_CPU" "$ARMS" "$PC_PREFLIGHT" >"$R/meta.txt"

# The receiver, detached on its host; its PID written by itself just before exec (never found by a pattern).
# shellcheck disable=SC2016 # expanded on the host
LAUNCH='rm -f '"$RD"'/rx.pid; setsid nohup bash -c "echo \$\$ > '"$RD"'/rx.pid; exec \"\$0\" \"\$@\"" '"$BIN"' rx "$@" > '"$RD"'/rx.log 2>&1 < /dev/null &
for i in $(seq 1 50); do [ -s '"$RD"'/rx.pid ] && break; sleep 0.1; done'

run_one() { # rep arm
    local rep=$1 arm=$2 stem status=ok rxrc state i
    stem="r${rep}_${arm}"
    rx_stop
    sh_ "$RECEIVER" "set -- $arm $PORT $ROUNDS $WARMUP $RX_CPU $TX_IP $ECHO_PORT $RD/rx.samples; rm -f $RD/rx.samples; $LAUNCH" </dev/null >/dev/null
    # Up: RXREADY in its log; refused: REFUSED there; anything else within 10 s is a failure to start.
    state=none
    for i in $(seq 1 50); do
        # shellcheck disable=SC2016 # expanded on the host
        state=$(sh_ "$RECEIVER" 'grep -m1 -oE "^(RXREADY|REFUSED)" '"$RD"'/rx.log 2>/dev/null' </dev/null) && [ -n "$state" ] && break
        state=none; sleep 0.2
    done
    : "$i"
    if [ "$state" = REFUSED ]; then
        status=REFUSED
    elif [ "$state" != RXREADY ]; then
        status="VOID:rx_not_ready"
    elif ! rx_alive; then
        status="VOID:rx_exe_not_$BIN"
    else
        sh_ "$SENDER" "timeout 300 $BIN tx $RX_IP $PORT $ROUNDS $WARMUP $TX_CPU $GAP_US $ECHO_PORT" </dev/null >"$R/${stem}_tx.log" 2>&1
        for _ in $(seq 1 50); do rx_alive || break; sleep 0.2; done
        if rx_alive; then status="VOID:rx_did_not_stop"; rx_stop; fi
    fi
    sh_ "$RECEIVER" "cat $RD/rx.log" </dev/null >"$R/${stem}_rx.log" 2>&1
    fetch_ "$RECEIVER" "$RD/rx.samples" "$R/${stem}.samples"
    if [ "$status" = ok ]; then
        rxrc=$(grep -m1 '^RXRESULT ' "$R/${stem}_rx.log")
        case "$rxrc" in "RXRESULT style=$arm "*) ;; *) status="VOID:no_rxresult_for_$arm" ;; esac
        grep -q '^TXRESULT status=ok ' "$R/${stem}_tx.log" || status="VOID:no_txresult"
    fi
    echo "RUN rep=$rep arm=$arm stem=$stem status=$status" >>"$R/index.txt"
    say "RUN $stem $status | $(grep -h -oE '^RXRESULT .*' "$R/${stem}_rx.log" | grep -oE ' (n|missing|bad_stamps|p50_ns|cpu_ns_per|drain_reads)=[0-9]+' | tr -d '\n')" \
        "| $(grep -h -oE ' (lost|rtt_p50_ns)=[0-9]+' "$R/${stem}_tx.log" | tr -d '\n')"
}

read -r -a ARMS_A <<<"$ARMS"; NA=${#ARMS_A[@]}
for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep/$REPS $(date -Is) ---"
    for i in $(seq 0 $((NA - 1))); do
        run_one "$rep" "${ARMS_A[$(((i + rep - 1) % NA))]}"
    done
    link_unshaped || { say "the link was shaped during rep $rep - stopping"; exit 1; }
done
python3 "$X/recv_wake_xhost_read.py" "$OUT" >"$OUT.verdict.txt" 2>&1
rc=$?
tee -a "$SUM" <"$OUT.verdict.txt"
say "RWX_DONE reader_rc=$rc $(date -Is)"
