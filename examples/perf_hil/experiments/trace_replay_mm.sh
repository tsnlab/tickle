#!/usr/bin/env bash
# trace_replay_mm.sh - release stage 3 (time-varying network -> v2.2), trace replay with Mahimahi
# (~/claude_reports/TIME_VARYING_EXPERIMENT_PLAN_2026-11-03.md section 7 item 3: trace replay is required and uses
# public datasets; section 8 item 3: it uses Mahimahi). 2026-11-03. The CURRENT build's writer/reader pair across a
# cellular link replayed by mm-link from a public trace, against a constant-rate link of that trace's mean rate (the
# STATIC arm) and against Mahimahi's delay alone (the clean control). Same node binaries, cells, metrics and RESULT
# lines as time_varying_netns.sh (read its header); trace_replay_read.py reads an OUT.
#
# MAHIMAHI. GPL: this harness calls its installed binaries (/usr/local/bin/mm-link, mm-delay, setuid root) and reads
# the traces it ships (/usr/local/share/mahimahi/traces); nothing of its source is copied. A trace line is one
# opportunity, at that millisecond, for one 1500-byte packet to leave the bottleneck queue; mm-link repeats the file
# when it ends. Its default queue is unbounded (no loss, only delay); MM_QUEUE sets another (both directions).
#
# TOPOLOGY (worked out from the code before the first run; the smoke must show discovery and the namespaces):
#   One private namespace O per run (`sudo -n ip netns add`, never this PC's default namespace, which reaches the rig
#   LAN). Inside O, as this user (mm-* refuse a real uid of root - "please run as non-root" - so `setpriv` drops to it
#   under `ip netns exec`; the binaries are setuid and need no sudo), ONE Mahimahi shell: mm-link UP DOWN (or mm-delay
#   D for the control). It makes a second namespace I behind a point-to-point TUN pair, routes I's default to O and
#   masquerades I's traffic in O's iptables. One node runs in O, the other in I:
#     DATA_DIR=down (default): the writer (ping / pub) in O, the reader (echo / sub) in I, the mobile side - the data
#     takes the trace's downlink, as a Mahimahi client inside fetches from a server outside. DATA_DIR=up swaps them.
#   1. Discovery crosses with the limited broadcast and no unicast peer configuration. Both nodes are given
#      -b 255.255.255.255. tickle.c resolve_links(): a link whose broadcast no interface owns is normal when it is the
#      limited broadcast (rmw_tickle's default), so the context is created; hal_linux.c tt_bind() binds the well-known
#      socket to INADDR_ANY and sends everything from the data socket. The kernel routes a limited broadcast out of the
#      default route and delivers one received on any interface locally, so I's announces go out of I's TUN (mm-link
#      forwards every IP packet) and are delivered to O's well-known socket, and O's go the other way once O has a
#      default route through its TUN, which this harness adds (`ip route`; a namespace with no route fails sendto()
#      with "Network is unreachable"). Peers are then reached at the source address and data port they are heard on
#      (hal_linux.h struct tt_hal.data_sock: upsert_peer() records that), I's real address: O routes it over the TUN,
#      and I's datagrams to O's own TUN address are delivered locally, so the masquerade (POSTROUTING) never rewrites
#      them. No NAT is on the path.
#   2. Why ONE Mahimahi shell, and netem for delay and loss on the link arms. Nesting (mm-delay D mm-loss ... mm-link
#      ... node) breaks TickLE in two ways, both from the code: (a) every shell between the two nodes forwards I's
#      datagrams, and a router never forwards a limited broadcast, so discovery ends at the first shell; (b) every
#      forwarding shell masquerades, and TickLE's flows are not request/reply pairs a NAT can map: announces go TO the
#      well-known port and FROM the data socket (struct tt_hal.sock is receive-only), so the outer node's answer from
#      its data port to the inner node's mapped data port matches no conntrack entry, is refused in the shell's
#      namespace, and the refusal's conntrack entry can then steal the port the inner node's own flow needed. A unicast
#      peer link (tt_DISCOVERY_OPTIONS, tt_LinkOptions.peer) does not help: it targets the well-known port, which no
#      mapping in a shell answers. So the path is exactly one shell, and a link arm's added delay and loss are netem on
#      both TUN devices (each node's egress, as time_varying_netns.sh shapes each node's own egress), applied through
#      `sudo -n ip netns exec` (`sudo -n` allows only ip; I has no name, so `ip netns attach` names it from its first
#      process's PID). The control is mm-delay alone, as asked; its delay is Mahimahi's, the link arms' netem's - the
#      control is the clean setup, the judged comparison is trace minus constant, which share the mechanism exactly.
#   Every node is started by a wrapper that writes its PID and `readlink /proc/self/ns/net` before it runs the node;
#   while the nodes run the harness reads /proc/<pid>/ns/net and /proc/<pid>/exe of each node PID and lists
#   `ip netns pids` of O and I, against this shell's own namespace (netns.txt, NSCHECK lines). A node in the default
#   namespace, or both nodes in one, voids the run, and the reader names it.
#
# ARMS (per trace key K of TRACES; the control once a rep):
#   delay      mm-delay MM_DELAY: delay alone, no rate limit, no loss - the clean CONTROL. It must lose nothing.
#   const_K    mm-link of constant traces at K's mean rate in each direction over the replayed window (the first
#              DUR + 15 s of the file, the whole file when shorter) + netem "delay MM_DELAY ms loss random MM_LOSS%" on
#              both TUNs. The STATIC arm.
#   trace_K    mm-link K.up K.down + the same netem. Judged as trace_K minus const_K.
#   TRACES (default "vzs tmd att"; key:File-name adds another of the installed set) - chosen 2026-11-03 from the
#   installed set (Winstein et al., NSDI 2013, "Saturator" recordings of US carriers, all taken by a mobile user; none
#   is labelled stationary). Measured on the files, first 40 s (what a run replays), down / up:
#     vzs  Verizon-LTE-short      5.3 / 7.4 Mbit/s, no 100 ms window without an opportunity up, 0.8% down, longest
#                                 gap 216 / 117 ms: the mildest LTE trace, the stand-in for a stationary user.
#     tmd  TMobile-LTE-driving    8.7 / 31 Mbit/s, CV of 100 ms rates 1.00 down, gaps to 1.06 s down: driving LTE.
#     att  ATT-LTE-driving-2016   4.6 / 2.1 Mbit/s, 8.5% / 24% empty 100 ms windows, gaps to 1.1 / 4.1 s: driving LTE
#                                 with outages (X4's disconnection, on recorded data).
#   rel_large (1 MiB at LARGE_HZ, default 0.25: 2.2 Mbit/s with headers) runs only where that is at most LARGE_LOAD_MAX
#   (0.5) of the data direction's mean rate; otherwise it is SKIPped, said in summary.txt and skips.txt (a link
#   below the offered load fails the static arm too, which is not this question).
#
# PRE-REGISTERED (written 2026-11-03 before the first run; trace_replay_read.py implements each in code and it - not
# this comment - decides; the limits are time_varying_read.py's own constants, imported, not copied):
#   Judged as trace_K minus const_K, per cell, on the reps pooled; STATIC when const_K already fails the same limit.
#   A/A (as V1/V2/V4): over the limit decides FAIL only when trace - const also exceeds the A/A spread (the same
#   quantity on the trace arm's odd reps against its even reps); inside it, NOISE. Fewer than TR_MIN_REPS (4) usable
#   reps of either arm: VOID, the values printed. The trace replays identically every rep, so reps differ by the
#   node start's offset into the trace (~1 s) and the PC alone.
#   T_COPIES (rel_small, rel_large; and rel_small's pong stream, T_COPIES_RETURN): copies after delivery per
#     delivered (rel_large per unit, 723 a sample) - trace minus const <= V1_COPIES_MAX (1%, R7/V1).
#   T_TIMER_PER_GAP (+_RETURN): the reader's retry-timer requests per gap opened <= X1_TIMER_PER_GAP_MAX (1.0).
#   T_RESEND_PER_MISSING (+_RETURN): units retransmitted per unit seen missing <= X1_RESEND_PER_MISSING_MAX (1.5).
#     Both need X1_MIN_GAPS (10) pooled, else N/A.
#   T_ONTIME (rel_small; V3's form): the share of offered samples delivered within the deadline (V3_DEADLINE_MS +
#     MM_DELAY: 100 ms + the added one-way delay, from creation) as a ratio to be_small's in the same arm - the channel
#     itself, which no protocol beats - trace >= (1 - V3_GOODPUT_DROP_MAX) = 90% of const's. Needs be_small.
#   Report only: DELIVERY and ABANDONED (KEEP_LAST 10 is 100 ms of history at 100 Hz: under a trace's gaps a repair
#     can come after eviction, by design), one-way p50/p99 per cell, round trip p50/p99, rel_small minus be_small
#     one-way p99, immediate requests per gap, rel_large ONTIME (no be_large to normalize by), the mm-link log's
#     realized rate and queueing delay, realized netem loss.
#   VOID (outcome, never config): a node outside its private namespace or both in one; a missing RESULT line, a
#     Publisher that never matched, under 60% scored, a short trace (time_varying_read.load_run's rules); the control
#     losing anything (BE < 99.9%, RELIABLE < 100% delivered, any qdisc drop: every verdict VOID) or resending on
#     rel_large (rel_large verdicts VOID); a treatment not seen - a run's one-way p50 under MM_DELAY - 0.5 ms - 5%
#     (the delay), a link arm's realized netem loss on the writer's TUN outside [0.5 L, 1.5 L], a const arm's mm-link
#     opportunity rate in the data direction off its nominal by more than 5%, or a trace arm whose largest mm-link
#     queueing delay in the data direction is not at least TRACE_SEEN_MS (10) above its const arm's (the trace's
#     variation not seen), or an unreadable mm-link log; a PC 1-minute load above nproc/2 at a run's start drops that
#     run from T_ONTIME.
#
# Usage: [TRACES="vzs tmd att"] [CELLS="be_small rel_small rel_large"] [REPS=4] [DUR=60] [MM_DELAY=20] [MM_LOSS=2]
#        [MM_QUEUE="droptail packets=1000"] [DATA_DIR=down|up] [LARGE_HZ=0.25] [PROFILE=ethernet|wifi]
#        [NODE_PIN="2 4 6"] [EST_TRACE=0|1] [NODE_ENV=...] [CONTROL=1] [OUT=dir] trace_replay_mm.sh
#   NODE_PIN: the writer, the reader and the Mahimahi shell each under taskset on the k-th CPU listed (X0a); "" runs
#   unpinned. EST_TRACE=1 builds with -Dtt_ESTIMATOR_TRACE (time_varying_netns.sh's X0d), stderr in each .err file.
#   CONTROL=0 leaves the delay arm out (a reader then voids everything; for a debugging run only).
#   OUT defaults to ~/rig_results_safe/trace_replay/<stamp>: raw logs per run in OUT/runs/<arm>-<cell>-<rep>/ (the
#   reader's layout), the summary in OUT/summary.txt, which always ends with "=== trace_replay_mm ended". READ_ONLY=1
#   re-reads an OUT. TIME (smoke 2026-11-03, DUR=30, vzs rel_small: 3 runs in 120 s with
#   the build): a run is DUR + ~8 s; with the defaults 7 arms x 3 cells = 21 runs a rep (fewer when rel_large is
#   skipped), ~24 min, REPS=4 ~1 h 40 min.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
TRACES=${TRACES:-"vzs tmd att"}
CELLS=${CELLS:-"be_small rel_small rel_large"}
REPS=${REPS:-4}
DUR=${DUR:-60}
MM_DELAY=${MM_DELAY:-20}
MM_LOSS=${MM_LOSS:-2}
MM_QUEUE=${MM_QUEUE:-}
TUN_TXQ=${TUN_TXQ:-10000} # each TUN's txqueuelen (see shape())
DATA_DIR=${DATA_DIR:-down}
PROFILE=${PROFILE:-ethernet}
NODE_PIN=${NODE_PIN-"2 4 6"}
EST_TRACE=${EST_TRACE:-0}
CONTROL=${CONTROL:-1}
SMALL_HZ=100
SMALL_BYTES=100
LARGE_BYTES=1048576
LARGE_HZ=${LARGE_HZ:-0.25}
LARGE_LOAD_MAX=0.5
LARGE_UNITS=723 # units of a 1 MiB sample at a 1500-byte MTU (time_varying_read.LARGE_UNITS_PER_SAMPLE)
TRACE_DIR=/usr/local/share/mahimahi/traces
MM_BIN=/usr/local/bin
BCAST=255.255.255.255
WIN_S=$((DUR + 15))
OUT=${OUT:-$HOME/rig_results_safe/trace_replay/$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT/runs" || exit 1
SUM="$OUT/summary.txt"
say() { echo "$*" | tee -a "$SUM"; }

if [ "${READ_ONLY:-0}" = 1 ]; then
    python3 "$HERE/trace_replay_read.py" "$OUT"
    exit $?
fi
: >"$SUM"

case "$EST_TRACE" in 0 | 1) ;; *) echo "EST_TRACE must be 0 or 1" >&2; exit 2 ;; esac
case "$PROFILE" in ethernet | wifi) ;; *) echo "PROFILE must be ethernet or wifi" >&2; exit 2 ;; esac
case "$DATA_DIR" in down | up) ;; *) echo "DATA_DIR must be down or up" >&2; exit 2 ;; esac
case "$CONTROL" in 0 | 1) ;; *) echo "CONTROL must be 0 or 1" >&2; exit 2 ;; esac
[[ $MM_DELAY =~ ^[0-9]+$ && $MM_LOSS =~ ^[0-9]+([.][0-9]+)?$ && $DUR =~ ^[0-9]+$ ]] ||
    { echo "MM_DELAY and DUR are integers, MM_LOSS a number" >&2; exit 2; }
TRACE_DEF=()
[ "$EST_TRACE" = 1 ] && TRACE_DEF=(-Dtt_ESTIMATOR_TRACE)
LARGE_PROFILE=()
[ "$PROFILE" = wifi ] && LARGE_PROFILE=(-P wifi)
QARGS=()
if [ -n "$MM_QUEUE" ]; then
    read -r qtype qargs <<<"$MM_QUEUE"
    QARGS=("--uplink-queue=$qtype" "--downlink-queue=$qtype")
    [ -n "${qargs:-}" ] && QARGS+=("--uplink-queue-args=$qargs" "--downlink-queue-args=$qargs")
fi
PIN=()
[ -n "$NODE_PIN" ] && read -r -a PIN <<<"$NODE_PIN"
[ -z "$NODE_PIN" ] || [ "${#PIN[@]}" = 3 ] || { echo "NODE_PIN lists three CPUs" >&2; exit 2; }
for b in mm-link mm-delay; do
    [ -u "$MM_BIN/$b" ] || { echo "$MM_BIN/$b is not installed setuid" >&2; exit 2; }
done

# trace_file KEY: the trace's file base name; keys are lower-case words (an arm name carries one).
declare -A TRACE_FILE=([vzs]=Verizon-LTE-short [tmd]=TMobile-LTE-driving [att]=ATT-LTE-driving-2016)
KEYS=""
for t in $TRACES; do
    key=${t%%:*}
    # [a-z0-9]+ and not starting with p: time_varying_read.load_run reads "_p" in an arm name as a time-varying arm
    [[ $key =~ ^[a-oq-z0-9][a-z0-9]*$ ]] || { echo "trace key '$key': [a-z0-9]+, not starting with p" >&2; exit 2; }
    [ "$t" != "$key" ] && TRACE_FILE[$key]=${t#*:}
    [ -n "${TRACE_FILE[$key]:-}" ] || { echo "unknown trace key $key (give key:File-name)" >&2; exit 2; }
    for d in up down; do
        [ -r "$TRACE_DIR/${TRACE_FILE[$key]}.$d" ] || { echo "no $TRACE_DIR/${TRACE_FILE[$key]}.$d" >&2; exit 2; }
    done
    KEYS="$KEYS $key"
done

WORK=$(mktemp -d "$HOME/tmp_build/trace_replay.XXXXXX") || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
LAUNCHED="$WORK/launched" # PIDs from $! of this shell's own launches (sudo), one a line
: >"$LAUNCHED"
# The one EXIT trap. Processes are found by namespace membership of the namespaces this run created (`ip netns pids`
# of a name this run made), never by a pattern; they are signalled inside their namespace as root, the only way the
# rules allow (`sudo -n ip`), since mm-link and dnsmasq run with root's effective uid.
on_exit() {
    local rc=$? ns pids
    while read -r ns; do
        [ -n "$ns" ] || continue
        pids=$(sudo -n ip netns pids "$ns" 2>/dev/null | tr '\n' ' ')
        # shellcheck disable=SC2086 # PIDs are words
        [ -n "${pids// /}" ] && sudo -n ip netns exec "$ns" kill $pids 2>/dev/null
        sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    [ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
    say "=== trace_replay_mm ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
DEFAULT_NS=$(readlink /proc/self/ns/net)
TREE_SHA=$(git -C "$REPO" rev-parse --short HEAD)
DIRTY=$(git -C "$REPO" status --porcelain -- src include platform | wc -l)
say "=== trace_replay_mm $(date -Is): tree=$TREE_SHA dirty_paths=$DIRTY PROFILE=$PROFILE REPS=$REPS DUR=$DUR" \
    "TRACES='$KEYS' CELLS='$CELLS' mm_delay=$MM_DELAY mm_loss=$MM_LOSS data_dir=$DATA_DIR ==="

# --- build: both binaries from this tree, and the monotonic stamp (as time_varying_netns.sh) -------------------------
CORE=("$REPO/src/tickle.c" "$REPO/src/encoding.c" "$REPO/src/log.c" "$REPO/src/hal_linux.c")
export TMPDIR=$HOME/tmp_build
if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS "${TRACE_DEF[@]}" -I"$REPO/include" \
    -I"$REPO/src" -I"$REPO/examples/perf_hil/tickle/common" -o "$WORK/node" "$HERE/wifi_link_node.c" "${CORE[@]}" \
    -lpthread -lm >"$WORK/build.log" 2>&1; then
    say "VOID  wifi_link_node did not build"
    tail -5 "$WORK/build.log"
    exit 1
fi
if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    -Dtt_RELIABLE_STATS -DR4_TRACE "${TRACE_DEF[@]}" -I"$REPO/include" -I"$REPO/src" \
    -I"$REPO/examples/perf_hil/tickle/common" \
    -o "$WORK/large" "$HERE/large_sample_bench.c" "${CORE[@]}" -lpthread -lm >>"$WORK/build.log" 2>&1; then
    say "VOID  large_sample_bench did not build"
    tail -5 "$WORK/build.log"
    exit 1
fi
cat >"$WORK/mono.c" <<'EOF'
#include <stdio.h>
#include <time.h>
int main(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("%llu\n", (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec);
    return 0;
}
EOF
if ! cc -O2 -o "$WORK/mono" "$WORK/mono.c" >>"$WORK/build.log" 2>&1; then
    say "VOID  the monotonic stamp did not build"
    exit 1
fi
MONO="$WORK/mono"

# --- the node wrapper: run inside a namespace, as this user. Its one argument is a job file (no word of the command
# line Mahimahi parses may start with '-': mm-link's getopt would take it). It records its namespace, waits for GO
# (the harness shapes the namespace first), runs the node, records the node's PID and exit, and holds the namespace
# (and so its TUN and qdisc counters) until HOLD exists. --------------------------------------------------------------
cat >"$WORK/wrap.sh" <<'EOF'
#!/bin/bash
PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
# shellcheck disable=SC1090
. "$1" || exit 2
echo "NETNS tag=$TAG pid=$$ ns=$(readlink /proc/self/ns/net) uid=$(id -u)" >>"$DIR/netns.txt"
n=0
while [ "$GO" != - ] && [ ! -e "$GO" ]; do
    sleep 0.05
    n=$((n + 1))
    [ "$n" -ge 600 ] && { echo "NETNS tag=$TAG no-go" >>"$DIR/netns.txt"; exit 3; }
done
ulimit -v 2000000
pin=()
[ "$CPU" != - ] && pin=(taskset -c "$CPU")
if [ "$ERR" = - ]; then
    "${pin[@]}" env $NODEENV "${CMD[@]}" >"$OUT" 2>&1 &
else
    "${pin[@]}" env $NODEENV "${CMD[@]}" >"$OUT" 2>"$ERR" &
fi
node=$!
echo "NODE tag=$TAG pid=$node" >>"$DIR/netns.txt"
wait "$node"
rc=$?
echo "NODE_END tag=$TAG rc=$rc" >>"$DIR/netns.txt"
n=0
while [ "$HOLD" != - ] && [ ! -e "$HOLD" ] && [ "$n" -lt 1200 ]; do
    sleep 0.1
    n=$((n + 1))
done
exit "$rc"
EOF
chmod +x "$WORK/wrap.sh"

# --- traces: the window mean rate of each direction, and the constant traces at those rates --------------------------
# window_bps FILE: bits per second over the first WIN_S s (the whole file, as mm-link loops it, when shorter).
window_bps() {
    awk -v w="$WIN_S" '{ n_all++; last = $1; if ($1 <= w * 1000) n++ }
        END { if (last <= w * 1000) printf "%.0f\n", n_all * 12000 / (last / 1000)
              else printf "%.0f\n", n * 12000 / w }' "$1"
}
# const_trace BPS FILE: a 1-second trace of round(BPS / 12000) evenly spaced opportunities (mm-link repeats it).
const_trace() {
    awk -v r="$1" 'BEGIN { p = int(r / 12000 + 0.5); if (p < 1) p = 1
        for (i = 1; i <= p; i++) { t = i * 1000 / p; ti = int(t); if (ti < t) ti++; print ti } }' >"$2"
}
declare -A RATE
for key in $KEYS; do
    for d in up down; do
        f="$TRACE_DIR/${TRACE_FILE[$key]}.$d"
        RATE[$key.$d]=$(window_bps "$f")
        const_trace "${RATE[$key.$d]}" "$WORK/const_$key.$d"
    done
done
{
    echo "tree=$TREE_SHA dirty_paths=$DIRTY profile=$PROFILE reps=$REPS dur=$DUR est_trace=$EST_TRACE"
    echo "traces='$KEYS' cells='$CELLS' mm_delay=$MM_DELAY mm_loss=$MM_LOSS mm_queue='$MM_QUEUE' tun_txq=$TUN_TXQ data_dir=$DATA_DIR"
    echo "small_bytes=$SMALL_BYTES small_hz=$SMALL_HZ large_bytes=$LARGE_BYTES large_hz=$LARGE_HZ nproc=$(nproc)"
    echo "large_load_max=$LARGE_LOAD_MAX win_s=$WIN_S node_pin='$NODE_PIN' node_env='${NODE_ENV:-}' control=$CONTROL"
    echo "build_small=cc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS${TRACE_DEF:+ ${TRACE_DEF[*]}}"
    echo "build_large=cc -O2 -DNDEBUG -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0" \
        "-Dtt_RELIABLE_STATS -DR4_TRACE${TRACE_DEF:+ ${TRACE_DEF[*]}}"
    for key in $KEYS; do
        echo "TRACE key=$key file=${TRACE_FILE[$key]} up_bps=${RATE[$key.up]} down_bps=${RATE[$key.down]}" \
            "const_up_lines=$(wc -l <"$WORK/const_$key.up") const_down_lines=$(wc -l <"$WORK/const_$key.down")" \
            "sha_up=$(sha256sum "$TRACE_DIR/${TRACE_FILE[$key]}.up" | cut -c1-16)" \
            "sha_down=$(sha256sum "$TRACE_DIR/${TRACE_FILE[$key]}.down" | cut -c1-16)"
    done
    echo "mm_link_bin=$(sha256sum "$MM_BIN/mm-link" | cut -c1-16)" \
        "mm_delay_bin=$(sha256sum "$MM_BIN/mm-delay" | cut -c1-16)"
    echo "default_netns=$DEFAULT_NS"
} >>"$OUT/provenance.txt"
: >"$OUT/skips.txt"

# --- one run -------------------------------------------------------------------------------------------------------
# job FILE TAG GO CPU OUT ERR HOLD CMD...: a wrapper job file (bash assignments, every value quoted by printf %q).
job() {
    local file=$1
    {
        printf 'TAG=%q\nDIR=%q\nGO=%q\nCPU=%q\nOUT=%q\nERR=%q\nHOLD=%q\nNODEENV=%q\n' "$2" "$RUN_DIR" "$3" "$4" "$5" \
            "$6" "$7" "${NODE_ENV:-}"
        printf 'CMD=('
        shift 7
        printf '%q ' "$@"
        printf ')\n'
    } >"$file"
}
# wait_line FILE REGEX SECONDS: until a line of FILE matches, at most SECONDS. Fails on the deadline.
wait_line() {
    local end=$(($(date +%s) + $3))
    until grep -qE "$2" "$1" 2>/dev/null; do
        [ "$(date +%s)" -ge "$end" ] && return 1
        sleep 0.1
    done
}
# tun_of NS: the namespace's one device other than lo (Mahimahi's TUN).
tun_of() {
    sudo -n ip -n "$1" -o link show | awk -F': ' '$2 != "lo" { sub(/@.*/, "", $2); print $2; exit }'
}
# shape NS DEV NETEM: DEV's queue to the Mahimahi process TUN_TXQ long (the default 500 dropped a 1 MiB sample's
# ~723-unit burst at the TUN on every arm, the control included: tx_dropped 13-1677 a run, full_20261103-231121), then
# the netem words as DEV's root qdisc (handle 10:); no qdisc when NETEM is empty (the control).
shape() {
    sudo -n ip -n "$1" link set dev "$2" txqueuelen "$TUN_TXQ" || return 1
    [ -n "$3" ] || return 0
    # shellcheck disable=SC2086 # the netem arguments are words on purpose
    sudo -n ip netns exec "$1" tc qdisc replace dev "$2" root handle 10: netem $3
}
# setup_link PID NETEM: names I from its first process (`ip netns attach`), finds both TUNs (sets odev and idev),
# shapes both, brings I's lo up and gives O a default route through its TUN (O's broadcasts need a route).
setup_link() {
    sudo -n ip netns attach "$I_NS" "$1" || return 1
    odev=$(tun_of "$O_NS")
    idev=$(tun_of "$I_NS")
    [ -n "$odev" ] && [ -n "$idev" ] || return 1
    shape "$O_NS" "$odev" "$2" && shape "$I_NS" "$idev" "$2" && sudo -n ip -n "$I_NS" link set lo up &&
        sudo -n ip -n "$O_NS" route replace default dev "$odev"
}
tun_counters() { # tun_counters NS DEV LABEL
    local ns=$1 dev=$2 label=$3 s=/sys/class/net/$2/statistics
    {
        echo "== $label dev=$dev tx_bytes=$(sudo -n ip netns exec "$ns" cat "$s/tx_bytes")" \
            "tx_packets=$(sudo -n ip netns exec "$ns" cat "$s/tx_packets")" \
            "tx_dropped=$(sudo -n ip netns exec "$ns" cat "$s/tx_dropped")" \
            "rx_packets=$(sudo -n ip netns exec "$ns" cat "$s/rx_packets")" \
            "rx_dropped=$(sudo -n ip netns exec "$ns" cat "$s/rx_dropped")"
        sudo -n ip netns exec "$ns" tc -s qdisc show dev "$dev"
    } >>"$RUN_DIR/counters.txt"
}
# nscheck TAG EXE: the node's PID (the wrapper's NODE line) - its namespace and binary read from /proc, its namespace
# against this shell's, and membership in `ip netns pids` of O and I. One NSCHECK line in netns.txt.
nscheck() {
    local tag=$1 exe=$2 pid ns real in_o=0 in_i=0 verdict=ok
    pid=$(awk -v t="tag=$tag" '$1 == "NODE" && $2 == t { sub(/pid=/, "", $3); print $3 }' "$RUN_DIR/netns.txt")
    if [ -z "$pid" ]; then
        echo "NSCHECK tag=$tag verdict=MISSING (no NODE line)" >>"$RUN_DIR/netns.txt"
        return
    fi
    ns=$(readlink "/proc/$pid/ns/net" 2>/dev/null || sudo -n ip netns exec "$O_NS" readlink "/proc/$pid/ns/net")
    real=$(readlink "/proc/$pid/exe" 2>/dev/null || sudo -n ip netns exec "$O_NS" readlink "/proc/$pid/exe")
    sudo -n ip netns pids "$O_NS" | grep -qx "$pid" && in_o=1
    sudo -n ip netns pids "$I_NS" | grep -qx "$pid" && in_i=1
    [ "$real" = "$(readlink -f "$exe")" ] || verdict=NOT_THE_NODE
    [ -n "$ns" ] || verdict=MISSING
    [ "$ns" = "$DEFAULT_NS" ] && verdict=DEFAULT_NETNS
    [ $((in_o + in_i)) = 1 ] || [ "$verdict" != ok ] || verdict=NOT_IN_O_OR_I
    echo "NSCHECK tag=$tag pid=$pid exe=$real ns=$ns default=$DEFAULT_NS in_o=$in_o in_i=$in_i verdict=$verdict" \
        >>"$RUN_DIR/netns.txt"
}

RUN_N=0
run_one() { # run_one ARM CELL REP
    local arm=$1 cell=$2 rep=$3 run="$1-$2-$3" kind key="" netem="" up down
    RUN_DIR="$OUT/runs/$run"
    RUN_N=$((RUN_N + 1))
    case "$arm" in
    delay) kind=delay ;;
    const_*) kind=link key=${arm#const_} up="$WORK/const_$key.up" down="$WORK/const_$key.down" ;;
    trace_*)
        kind=link key=${arm#trace_}
        up="$TRACE_DIR/${TRACE_FILE[$key]}.up" down="$TRACE_DIR/${TRACE_FILE[$key]}.down"
        ;;
    *) say "VOID  $run: unknown arm"; return 1 ;;
    esac
    if [ "$cell" = rel_large ] && [ "$kind" = link ]; then
        local need ok
        need=$(awk -v hz="$LARGE_HZ" -v u="$LARGE_UNITS" 'BEGIN { printf "%.0f", hz * u * 1500 * 8 }')
        ok=$(awk -v n="$need" -v r="${RATE[$key.$DATA_DIR]}" -v m="$LARGE_LOAD_MAX" 'BEGIN { print (n <= m * r) }')
        if [ "$ok" != 1 ]; then
            echo "SKIP $run: rel_large offers $need bit/s, over $LARGE_LOAD_MAX of $key's $DATA_DIR rate" \
                "${RATE[$key.$DATA_DIR]} bit/s" | tee -a "$SUM" >>"$OUT/skips.txt"
            return 0
        fi
    fi
    mkdir -p "$RUN_DIR"
    [ "$kind" = link ] && netem="limit 10000 delay ${MM_DELAY}ms"
    [ "$kind" = link ] && [ "$MM_LOSS" != 0 ] && netem="$netem loss random ${MM_LOSS}%"
    echo "load=$(cut -d' ' -f1 /proc/loadavg) nproc=$(nproc) start=$(date -Is) tv=0 kind=$kind trace_key=${key:--}" \
        "mm_delay=$MM_DELAY netem='$netem' data_dir=$DATA_DIR rate_up=${RATE[$key.up]:--}" \
        "rate_down=${RATE[$key.down]:--} node_pin='$NODE_PIN' est_trace=$EST_TRACE" >"$RUN_DIR/meta.txt"
    O_NS="trr$$-$RUN_N-o"
    I_NS="trr$$-$RUN_N-i"
    echo "$O_NS" >>"$NS_LIST"
    if ! { sudo -n ip netns add "$O_NS" && sudo -n ip -n "$O_NS" link set lo up; } >"$RUN_DIR/topo.log" 2>&1; then
        say "VOID  $run: no namespace (see $RUN_DIR/topo.log)"
        return 1
    fi
    local ons
    ons=$(sudo -n ip netns exec "$O_NS" readlink /proc/self/ns/net)
    if [ -z "$ons" ] || [ "$ons" = "$DEFAULT_NS" ]; then
        say "VOID  $run: namespace $O_NS reads as '$ons', this shell's is $DEFAULT_NS - not starting Mahimahi"
        return 1
    fi
    echo "o_ns=$O_NS $ons ip_forward=$(sudo -n ip netns exec "$O_NS" cat /proc/sys/net/ipv4/ip_forward)" \
        >>"$RUN_DIR/topo.log"
    # roles: the writer and reader commands, and which side each runs on
    local mode=be stop="$WORK/stop-$run" go="$WORK/go-$run" hold="$WORK/hold-$run" w_cmd r_cmd w_exe r_exe
    rm -f "$stop" "$go" "$hold"
    [ "$cell" != be_small ] && mode=rel
    local r_err=- w_err=-
    if [ "$cell" = rel_large ]; then
        w_exe="$WORK/large" r_exe="$WORK/large" r_err="$RUN_DIR/echo.err" w_err="$RUN_DIR/ping.err"
        w_cmd=("$WORK/large" pub -s "$LARGE_BYTES" -r "$LARGE_HZ" -d "$DUR" -R -b "$BCAST" "${LARGE_PROFILE[@]}")
        r_cmd=("$WORK/large" sub -s "$LARGE_BYTES" -d $((DUR + 8)) -R -b "$BCAST" "${LARGE_PROFILE[@]}")
    else
        w_exe="$WORK/node" r_exe="$WORK/node"
        [ "$EST_TRACE" = 1 ] && r_err="$RUN_DIR/echo.err" w_err="$RUN_DIR/ping.err"
        w_cmd=("$WORK/node" ping -c 1 -d "$DUR" -m "$mode" -s "$SMALL_BYTES" -r "$SMALL_HZ" -l "$RUN_DIR/rtt_us.txt"
            -t "$RUN_DIR/trace_ping.txt" -b "$BCAST" -p "$PROFILE" -q "$stop")
        r_cmd=("$WORK/node" echo -c 2 -d $((DUR + 60)) -m "$mode" -s "$SMALL_BYTES" -r "$SMALL_HZ"
            -l "$RUN_DIR/owd_us.txt" -t "$RUN_DIR/trace_echo.txt" -b "$BCAST" -p "$PROFILE" -q "$stop")
    fi
    local cw=- cr=- cm=()
    [ -n "$NODE_PIN" ] && cw=${PIN[0]} cr=${PIN[1]} cm=(taskset -c "${PIN[2]}")
    # inner side: the reader with DATA_DIR=down, the writer with up; the outer side the other
    local in_tag in_job="$WORK/job-in-$run" out_job="$WORK/job-out-$run"
    if [ "$DATA_DIR" = down ]; then
        in_tag="echo"
        job "$in_job" echo "$go" "$cr" "$RUN_DIR/echo.log" "$r_err" "$hold" "${r_cmd[@]}"
        job "$out_job" ping - "$cw" "$RUN_DIR/ping.log" "$w_err" - "${w_cmd[@]}"
    else
        in_tag="ping"
        job "$in_job" ping "$go" "$cw" "$RUN_DIR/ping.log" "$w_err" "$hold" "${w_cmd[@]}"
        job "$out_job" echo - "$cr" "$RUN_DIR/echo.log" "$r_err" - "${r_cmd[@]}"
    fi
    local mm=()
    if [ "$kind" = delay ]; then
        mm=("$MM_BIN/mm-delay" "$MM_DELAY")
    else
        mm=("$MM_BIN/mm-link" "${QARGS[@]}" "--uplink-log=$RUN_DIR/mm_up.log" "--downlink-log=$RUN_DIR/mm_down.log"
            "$up" "$down")
    fi
    local as_me=(sudo -n ip netns exec "$O_NS" setpriv --reuid "$(id -u)" --regid "$(id -g)" --init-groups)
    local t0 out_pid mm_pid
    t0=$("$MONO")
    echo "PHASE idx=0 key=$arm t_before_ns=$t0 t_after_ns=$t0 ok=1" >"$RUN_DIR/phases.txt"
    : >"$RUN_DIR/netns.txt"
    if [ "$DATA_DIR" = up ]; then # the reader (outside) first, as time_varying_netns.sh starts echo first
        "${as_me[@]}" "$WORK/wrap.sh" "$out_job" >>"$RUN_DIR/topo.log" 2>&1 &
        out_pid=$!
        echo "$out_pid" >>"$LAUNCHED"
        sleep 1
    fi
    "${as_me[@]}" "${cm[@]}" "${mm[@]}" "$WORK/wrap.sh" "$in_job" >"$RUN_DIR/mm.log" 2>&1 &
    mm_pid=$!
    echo "$mm_pid" >>"$LAUNCHED"
    if ! wait_line "$RUN_DIR/netns.txt" "^NETNS tag=$in_tag " 20; then
        say "VOID  $run: the Mahimahi shell never ran its command (see $RUN_DIR/mm.log)"
        touch "$hold" "$stop"
        return 1
    fi
    local in_pid odev idev
    in_pid=$(awk -v t="tag=$in_tag" '$1 == "NETNS" && $2 == t { sub(/pid=/, "", $3); print $3 }' "$RUN_DIR/netns.txt")
    echo "$I_NS" >>"$NS_LIST"
    local shaped=0
    setup_link "$in_pid" "$netem" >>"$RUN_DIR/topo.log" 2>&1 || shaped=1
    {
        echo "odev=${odev:-} idev=${idev:-} in_pid=$in_pid i_ns=$I_NS shaped_rc=$shaped"
        echo "-- O addr/route/qdisc"
        sudo -n ip -n "$O_NS" -o addr show
        sudo -n ip -n "$O_NS" route show
        [ -n "${odev:-}" ] && sudo -n ip netns exec "$O_NS" tc qdisc show dev "$odev"
        echo "-- I addr/route/qdisc"
        sudo -n ip -n "$I_NS" -o addr show
        sudo -n ip -n "$I_NS" route show
        [ -n "${idev:-}" ] && sudo -n ip netns exec "$I_NS" tc qdisc show dev "$idev"
    } >>"$RUN_DIR/topo.log" 2>&1
    if [ "$shaped" != 0 ]; then
        say "VOID  $run: the namespaces could not be set up (see $RUN_DIR/topo.log)"
        touch "$hold" "$stop"
        return 1
    fi
    touch "$go"
    if [ "$DATA_DIR" = down ]; then # the reader (inside) has started; the writer a second later
        wait_line "$RUN_DIR/netns.txt" "^NODE tag=$in_tag " 10
        sleep 1
        "${as_me[@]}" "$WORK/wrap.sh" "$out_job" >>"$RUN_DIR/topo.log" 2>&1 &
        out_pid=$!
        echo "$out_pid" >>"$LAUNCHED"
    fi
    sleep 2
    nscheck ping "$w_exe"
    nscheck echo "$r_exe"
    {
        echo "-- ip netns pids $O_NS: $(sudo -n ip netns pids "$O_NS" | tr '\n' ' ')"
        echo "-- ip netns pids $I_NS: $(sudo -n ip netns pids "$I_NS" | tr '\n' ' ')"
    } >>"$RUN_DIR/topo.log"
    wait_line "$RUN_DIR/netns.txt" "^NODE_END tag=ping " $((DUR + 90)) || say "WARN  $run: the writer did not end"
    if [ "$cell" = rel_large ]; then
        wait_line "$RUN_DIR/netns.txt" "^NODE_END tag=echo " 40 || say "WARN  $run: the reader did not end"
    else
        sleep 1
        touch "$stop"
        wait_line "$RUN_DIR/netns.txt" "^NODE_END tag=echo " 70 || say "WARN  $run: the reader did not end"
    fi
    tun_counters "$O_NS" "$odev" out
    tun_counters "$I_NS" "$idev" in
    touch "$hold"
    wait "$out_pid" 2>/dev/null
    local n=0
    while [ -d "/proc/$mm_pid" ] && [ "$n" -lt 100 ]; do
        sleep 0.1
        n=$((n + 1))
    done
    [ -d "/proc/$mm_pid" ] && say "WARN  $run: the Mahimahi shell outlived its command; the trap ends it"
    wait "$mm_pid" 2>/dev/null
    sudo -n ip netns del "$I_NS" 2>/dev/null
    sudo -n ip netns del "$O_NS" 2>/dev/null
    local line ns_bad
    ns_bad=$(grep '^NSCHECK' "$RUN_DIR/netns.txt" | grep -vc 'verdict=ok$')
    line=$(grep -h '^RESULT' "$RUN_DIR/ping.log" "$RUN_DIR/echo.log" 2>/dev/null | tr '\n' ' ' | cut -c1-300)
    say "$run: nscheck_bad=$ns_bad ${line:-<no RESULT line>}"
}

ARMS=""
[ "$CONTROL" = 1 ] && ARMS="delay"
for key in $KEYS; do ARMS="$ARMS const_$key trace_$key"; done
for rep in $(seq 1 "$REPS"); do
    for cell in $CELLS; do
        for arm in $ARMS; do
            run_one "$arm" "$cell" "$rep"
        done
    done
done
say "--- reading (trace_replay_read.py)"
python3 "$HERE/trace_replay_read.py" "$OUT" | tee -a "$SUM"
