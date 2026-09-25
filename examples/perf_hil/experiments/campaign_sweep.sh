#!/usr/bin/env bash
# The rig performance campaign: five metrics, three frameworks, one session.
# Implements examples/perf_hil/OPTIMIZATION_PLAN.md rev 4. Read that first - this script is the
# mechanism, the plan is the reasoning, and the reading rules below come from its section 9.
#
# 12 combinations x 3 frameworks x 3 repetitions = 108 runs, ~45 min, plus ~8 min of builds.
# Frameworks are interleaved within each repetition so a drift during the session lands on all
# three rather than on whichever ran last.
#
# HOW TO READ IT, written before running (OPTIMIZATION_PLAN.md section 9):
#   - instrument=ok is required. A counter reading zero sets instrument=fail:<which> on the harness
#     side; idle eth0 is a measured byte-identical zero delta, so a zero byte count cannot be a quiet
#     link. Any cell without instrument=ok is VOID.
#   - The boundary gate: at N0, the CLIENT's own client.wire_role_packets_per_sample must read
#     below 1.5 ("one datagram") for all three at P1 and P2, below 1.5 for TickLE and at or above
#     2.0 for both vendors at P3, and at or above 2.0 for all three at P4. Three things about the
#     shape of that rule, all of which cost a wrong version first:
#       * it reads the role-split client metric, not wire_packets_per_sample, which counts the whole
#         interface in both directions and so carries the returning ACKNACKs on a RELIABLE publisher
#         (TickLE Dev's correction);
#       * it is a band and not an equality, because a publisher also sends heartbeats, so a correct
#         single datagram reads slightly above 1.0 and never exactly 1.0;
#       * it is a gate, not a metric to win - at P1/P2 all three are meant to read the same, and
#         calling that three-way equality a TickLE draw would be a verdict on the test design.
#     RTPS framing is spec arithmetic, not a measurement, and this is what checks it. A size that
#     misses its intended split makes that size's cross-vendor comparison VOID - the numbers are
#     still printed, with the verdict, because the observed packet count is how the right size gets
#     computed next time.
#   - A cell is also VOID with no RESULT line, a fired leftover guard, or a framework that did not
#     end drained=acked where that is expected.
#   - TickLE wins a cell only by beating BOTH vendors outside the spread of the three repetitions.
#     Inside the spread is a draw.
#   - Expected, recorded so neither reads as a surprise: TickLE should LOSE cell 6 (P4 under loss),
#     because a lost IP fragment costs it the whole datagram while RTPS retransmits one fragment.
#
# DRY_RUN=1 prints the plan and the tc sequence without touching the rig or holding the lock.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PH/../.." && pwd)"
REPS="${REPS:-3}"
DUR="${DUR:-5}"
DRY_RUN="${DRY_RUN:-0}"
OUT="${OUT:-/tmp/tickle_campaign_$(date +%Y%m%d-%H%M%S).txt}"

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT=10.1.1.214
RPI_SERVER=10.1.1.213
HOSTS=("$RPI_CLIENT" "$RPI_SERVER")

if [ "$DRY_RUN" != 1 ] && [ "${RIG_LOCK_HELD_HIL:-${RIG_LOCK_HELD:-0}}" != "1" ]; then
    OUT="$OUT" exec "$PH/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
ssh_h() { ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }

# --- network conditions (OPTIMIZATION_PLAN.md section 6) --------------------------------------
# Applied on the client's eth0, which is also the measured and the data interface. reorder needs a
# delay to have anything to reorder against, hence N3's 1ms.
# `tc qdisc del root` legitimately fails when there is nothing to delete, so its exit status cannot
# be the check - which is why it was `|| true`, and why a real failure (no NOPASSWD for tc, the same
# shape as the sudo/tcpdump hole of 2026-09-24) would have left netem in place silently and shaped
# every later measurement on this rig, including CI's. The qdisc is read back instead.
tc_netem_present() { ssh_h "$RPI_CLIENT" "tc qdisc show dev eth0" 2>/dev/null | grep -q netem; }
tc_apply() {
    case "$1" in
        N0) ssh_h "$RPI_CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true
            if tc_netem_present; then
                echo "FATAL: netem still on $RPI_CLIENT eth0 after del - the rig is left shaped" >&2
                ssh_h "$RPI_CLIENT" "tc qdisc show dev eth0" >&2 || true
                return 1
            fi ;;
        N1) ssh_h "$RPI_CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss 5%" ;;
        N2) ssh_h "$RPI_CLIENT" "sudo -n tc qdisc replace dev eth0 root netem delay 10ms 2ms" ;;
        N3) ssh_h "$RPI_CLIENT" "sudo -n tc qdisc replace dev eth0 root netem delay 1ms reorder 5% 50%" ;;
        *)  echo "unknown network condition $1" >&2; return 1 ;;
    esac
}
tc_describe() {
    case "$1" in
        N0) echo "none" ;; N1) echo "loss 5%" ;;
        N2) echo "delay 10ms jitter 2ms" ;; N3) echo "delay 1ms reorder 5%" ;;
    esac
}
trap 'tc_apply N0 || echo "RIG LEFT SHAPED - clear it before any further measurement" >&2' EXIT

# The EXIT trap covers a normal exit and SIGTERM (verified by driving a copy: SIGTERM ran the trap,
# SIGKILL did not, because SIGKILL cannot be caught). So a SIGKILLed run leaves netem behind, and
# the only place that can be caught is the start of the next one. Refusing is deliberate: silently
# clearing it would hide that some earlier run died holding the rig shaped.
if [ "$DRY_RUN" != 1 ] && tc_netem_present; then
    say "REFUSING TO START: $RPI_CLIENT eth0 already has netem on it, so N0 would not be 'no shaping'."
    ssh_h "$RPI_CLIENT" "tc qdisc show dev eth0" | tee -a "$OUT"
    say "A previous run was probably SIGKILLed. Clear it and re-run:"
    say "  ssh -i $SSH_KEY ci@$RPI_CLIENT 'sudo -n tc qdisc del dev eth0 root'"
    exit 1
fi

# --- the matrix (OPTIMIZATION_PLAN.md section 7) ------------------------------------------------
# shape|payload|qos|network|scenario|tickle_extra_args
# Q0 = RELIABLE + KEEP_ALL: -Q for TickLE, the DDS harnesses' own default. The only configuration
#      in which all three promise the same thing (section 5).
# Q1 = BEST_EFFORT, which is a different scenario binary rather than a flag.
# Q2 = RELIABLE + KEEP_LAST 64: TickLE's default, so no flag. Cross-vendor only once both DDS
#      harnesses can select it; until then this cell is a TickLE datapoint, which the summary says.
MATRIX=(
    "T|p1|Q0|N0|reliable_throughput|-Q"
    "T|p2|Q0|N0|reliable_throughput|-Q"
    "T|p3|Q0|N0|reliable_throughput|-Q"
    "T|p4|Q0|N0|reliable_throughput|-Q"
    "T|p1|Q0|N1|reliable_throughput|-Q"
    "T|p4|Q0|N1|reliable_throughput|-Q"
    "T|p1|Q0|N3|reliable_throughput|-Q"
    "T|p1|Q1|N0|best_effort_throughput|"
    "T|p1|Q2|N0|reliable_throughput|"
    "L|p1|Q0|N0|reliable_latency|"
    "L|p2|Q0|N0|reliable_latency|"
    "L|p1|Q0|N2|reliable_latency|"
)
# Scenario/payload pairs that have to exist as built directories.
needed_variants() {
    local spec
    for spec in "${MATRIX[@]}"; do
        IFS='|' read -r _shape payload _qos _net scenario _extra <<<"$spec"
        echo "${scenario}_${payload}"
    done | sort -u
}

say "=== campaign sweep, $(date -Is), OPTIMIZATION_PLAN.md rev 4 ==="
say "repo $(git -C "$REPO" rev-parse --short origin/main), ${REPS} reps, -d ${DUR}, out $OUT"
say ""
say "--- plan: ${#MATRIX[@]} combinations x 3 frameworks x ${REPS} reps = $(( ${#MATRIX[@]} * 3 * REPS )) runs ---"
i=0
for spec in "${MATRIX[@]}"; do
    IFS='|' read -r shape payload qos net scenario extra <<<"$spec"
    i=$((i + 1))
    say "$(printf '%2d  %s  %-2s  %-2s  %-24s  %-22s  %s' \
        "$i" "$shape" "$payload" "$qos" "$scenario" "$(tc_describe "$net")" "${extra:-(no extra args)}")"
done
say ""
say "--- variants to build: $(needed_variants | paste -sd' ') ---"
if [ "$DRY_RUN" = 1 ]; then
    say "DRY_RUN=1: nothing was run, the rig was not touched and no lock was taken."
    exit 0
fi

# --- build every variant once, up front (section 7's time budget depends on this) ---------------
SHA="$(git -C "$REPO" rev-parse origin/main)"
say "--- deploying $SHA and building $(needed_variants | wc -l) variant(s) x 3 frameworks on both rpis ---"
VARIANTS="$(needed_variants | paste -sd' ')"
# `wait` with no arguments returns 0 however the background jobs ended, so a BUILD FAILED on either
# rpi would print and the sweep would carry on through 108 cells against a stale or missing build.
# Each PID is waited on individually, and then the binaries are checked for existence - the exit
# code says the builder thinks it succeeded, the check says the binaries are there.
pids=()
for host in "${HOSTS[@]}"; do
    ssh_h "$host" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx
cd examples/perf_hil
for v in $VARIANTS; do
  scen=\${v%_p[0-9]}; size=\${v##*_}
  for fw in tickle cyclonedds fastdds; do
    (cd \$fw && ./build.sh \$scen \$size >/tmp/campaign_build_\${fw}_\$v.log 2>&1) \
      || { echo \"BUILD FAILED: \$fw \$scen \$size\"; tail -8 /tmp/campaign_build_\${fw}_\$v.log; exit 1; }
  done
done
echo \"built on \$(hostname) at \$(git -C ~/tickle rev-parse --short HEAD)\"" &
    pids+=($!)
done
build_failed=0
for pid in "${pids[@]}"; do wait "$pid" || build_failed=1; done
[ "$build_failed" = 0 ] || { say "BUILD FAILED on at least one rpi - see the output above. Not running."; exit 1; }

# The binaries themselves, independent of any exit code. A DDS harness that has never been compiled
# anywhere (true of these sources until this runs) fails here rather than turning into 108 void cells.
# One ssh per host, not one per binary: the list is built remotely. run_scenario.sh in all three
# frameworks runs `./client`/`./server` from ~/tickle/examples/perf_hil/<fw>/<scenario>_<pN>/, so
# these are the exact paths the sweep is about to execute, not a guess at what a build emits.
missing=""
for host in "${HOSTS[@]}"; do
    absent="$(ssh_h "$host" "for v in $VARIANTS; do for fw in tickle cyclonedds fastdds; do
  for b in client server; do d=~/tickle/examples/perf_hil/\$fw/\$v/\$b
    [ -x \"\$d\" ] || echo \"\$fw/\$v/\$b\"; done; done; done" 2>&1)"
    [ -z "$absent" ] || missing+="$host: $(tr '\n' ' ' <<<"$absent")"
done
[ -z "$missing" ] || { say "MISSING BINARIES after a build that reported success: $missing"; exit 1; }
say "all $(( $(wc -w <<<"$VARIANTS") * 3 * 2 )) binaries present on both rpis"
say "client at $(ssh_h "$RPI_CLIENT" 'git -C ~/tickle rev-parse --short HEAD'), server at $(ssh_h "$RPI_SERVER" 'git -C ~/tickle rev-parse --short HEAD')"

# --- leftover guard: identify by /proc/PID/exe, never by a name pattern (CLAUDE.md rule 3) ------
rig_leftovers() {
    local host out=""
    for host in "${HOSTS[@]}"; do
        # shellcheck disable=SC2016 # expanded on the rpi, not here
        out+=$(ssh_h "$host" '
            for d in /proc/[0-9]*; do
                e=$(readlink "$d/exe" 2>/dev/null) || continue
                case "$e" in */tickle/examples/perf_hil/*/client|*/tickle/examples/perf_hil/*/server)
                    fw=${e%/*/*}; fw=${fw##*/}; printf "%s:%s:%s " "$(hostname)" "$fw" "${d#/proc/}";;
                esac
            done' 2>/dev/null)
    done
    printf '%s' "$out"
}
wait_rig_quiet() {
    local left _
    for _ in $(seq 1 30); do
        left=$(rig_leftovers); [ -z "$left" ] && { echo none; return 0; }
        sleep 1
    done
    echo "$left"
}

# --- one cell --------------------------------------------------------------------------------
# $1 combination number, $2 shape, $3 payload, $4 qos, $5 network, $6 scenario, $7 extra, $8 fw, $9 rep
cell() {
    local num=$1 shape=$2 payload=$3 qos=$4 net=$5 scenario=$6 extra=$7 fw=$8 rep=$9
    local left; left=$(wait_rig_quiet)
    local args="-d $DUR"
    [ "$shape" = L ] && args="-i 0.1 -d 10"
    # TickLE's extra args select KEEP_ALL; the DDS harnesses have it as their default (section 5).
    [ "$fw" = tickle ] && [ -n "$extra" ] && args="$extra $args"
    local res
    # shellcheck disable=SC2086 # $args is deliberately word-split into the harness's argv
    res=$(cd "$PH/$fw" && timeout 180 ./run_scenario.sh "${scenario}_${payload}" $args 2>/dev/null \
        | grep '^RESULT:' | tr '\n' ' ' | sed -E 's/RESULT: framework=[a-z]+ scenario=[a-z_0-9]+ //g' || true)
    local verdict=ok
    [ -n "$res" ] || verdict="VOID(no RESULT line)"
    [ "$left" = none ] || verdict="VOID(leftover $left)"
    case "$res" in *instrument=ok*) ;; *instrument=fail*) verdict="VOID($(grep -oE 'instrument=fail:[a-z,]+' <<<"$res" | head -1))" ;;
        *) [ -n "$res" ] && verdict="VOID(no instrument= field)" ;; esac
    say "$(printf 'c%-2s %s %-2s %-2s %-22s %-10s rep%s | %s | %s' \
        "$num" "$shape" "$payload" "$qos" "$(tc_describe "$net")" "$fw" "$rep" "$verdict" "${res:-none}")"
}

say ""
say "--- runs ---"
current_net=""
for rep in $(seq 1 "$REPS"); do
    num=0
    for spec in "${MATRIX[@]}"; do
        IFS='|' read -r shape payload qos net scenario extra <<<"$spec"
        num=$((num + 1))
        if [ "$net" != "$current_net" ]; then
            tc_apply "$net"; current_net="$net"
            say "  [tc: $(tc_describe "$net")]"
        fi
        for fw in tickle cyclonedds fastdds; do
            cell "$num" "$shape" "$payload" "$qos" "$net" "$scenario" "$extra" "$fw" "$rep"
        done
    done
done
tc_apply N0
say ""
say "=== done $(date -Is); tc restored. Summarise with campaign_summary.py, then read against section 9. ==="
