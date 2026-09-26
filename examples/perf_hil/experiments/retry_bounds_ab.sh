#!/usr/bin/env bash
# A/B for 08e568af: interval = srtt + max(G, 4*rttvar) with ceiling 64*srtt, replacing the fixed
# [250us, 10ms] clamp. Proposed in examples/perf_hil/CONSTANTS_AUDIT.md, corrected by TickLE Dev
# (a floor of 1*srtt can never bind, since interval >= srtt identically; RFC 6298's own max(G, ...)
# is the right shape and G is a host property, not a link one).
#
# HOW TO READ IT, written before running:
#   ARM 1, unshaped. The old clamps do not bind at srtt ~200us, so the prediction is NO DIFFERENCE
#     in throughput or RTT. The one legitimate exception Dev named: where 4*rttvar < G the new
#     interval is slightly LONGER than the old max(250us, srtt+4*rttvar), so a small difference is
#     allowed only if it correlates with low recovery_rttvar_ns. A difference at HIGH rttvar would
#     mean the clamps were binding when both of us believed they were not - the more interesting
#     outcome, and the reason this arm exists at all rather than only arm 2.
#   ARM 2, netem delay 20ms PLUS loss 5%, is the arm that tests the claim. The first version of
#     this script used delay alone, which injects no loss: 0-1 retransmissions per 61,000 samples,
#     so the arm meant to exercise the retry ceiling had nothing to time. Delay pushes true recovery
#     (~40ms) above the OLD 10ms ceiling; loss creates the recoveries to measure.
#     Prediction: BEFORE shows retry storms - retransmitted per recovered sample >> 1 and
#     wire_bytes_per_sample up; AFTER shows ~1. If BEFORE does NOT storm, the generality argument
#     for the change is unsupported on this rig and must be reported as such, not quietly dropped.
#
#   WHAT THIS ARM CAN AND CANNOT SEPARATE, stated before running. AFTER carries two changes: the
#   relative ceiling (08e568af) and the estimator fix (37190fda, which stops a sample that overtook
#   the original on the other socket being timed as a recovery - Karn's ambiguity). They are not
#   separable here, and that is a property of the defect rather than a shortcut: on a delayed link
#   the contaminated estimator is *itself* what drove srtt to tens of microseconds, and a ceiling
#   of 64*srtt on a contaminated srtt is tighter than the fixed 10ms it replaced. So this measures
#   the change as shipped against the behaviour as shipped, which is the question that matters, and
#   not the ceiling in isolation, which on this link is not a question that has an answer.
#   DIAGNOSTIC that shows the mechanism directly: recovery_srtt_ns. BEFORE should read tens of us
#   under 20ms of injected one-way delay - about a thousandfold too small, which is the
#   contamination. AFTER should read ~40ms, i.e. the real round trip. If AFTER still reads under
#   tt_RELIABLE_RETRY_GRANULARITY (100us) the estimator fix is not in the build.
#   CONTROL against my own harness: both arms assert core_build=release AND retry_interval_cfg_ns=0
#     on every RESULT line. The change only acts in dynamic mode, so an arm that silently ran fixed
#     retry would show "no difference" for the wrong reason - which is exactly how an earlier -O2
#     arm in this directory came back VOID.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
OUT=${OUT:-/tmp/retry_bounds_ab_$(date +%Y-%m-%d).txt}
BEFORE=${BEFORE:-ba76dfbf}; AFTER=${AFTER:-$(git -C "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)" rev-parse --short origin/main)}
REPS=${REPS:-3}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
server_pids() {
    # shellcheck disable=SC2016  # $p must expand on the rig
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */perf_hil/tickle/*/server) echo "$p";; esac; done'
}
kill_servers() { local p; p=$(server_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }

# netem: set, then READ BACK. `tc qdisc del || true` swallowing a real failure is what let a shaped
# link survive into a later run in this directory; the trap restores fq_codel on any exit path.
set_delay() {
    local ms="$1"
    # Verbatim from campaign_sweep.sh's own N0/N2 handling (sudo -n, del-root for unshaped) rather
    # than re-invented here - a made-up variant of a rig command has already cost this directory one
    # whole comparison.
    if [ "$ms" = "0" ]; then
        sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true
    else
        sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem delay $ms" >/dev/null 2>&1
    fi
    local q; q=$(sh_ "$CLIENT" "tc qdisc show dev eth0 | head -1")
    say "  qdisc now: $q"
    if [ "$ms" = "0" ]; then
        case "$q" in *netem*) say "  NETEM STILL PRESENT - stopping"; exit 1;; esac
    else
        case "$q" in *"delay 20ms"*) ;; *) say "  NETEM NOT APPLIED - stopping"; exit 1;; esac
        case "$q" in *"loss 5%"*) ;; *) say "  NETEM LOSS NOT APPLIED - stopping"; exit 1;; esac
    fi
}
cleanup() { kill_servers; set_delay 0 >/dev/null 2>&1 || true; }
trap cleanup EXIT

: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== relative retry bounds A/B, $(date -Is) ==="
say "before=$BEFORE (fixed [250us,10ms], contaminated estimator)   after=$AFTER (srtt+max(G,4rttvar), ceiling 64*srtt, Karn fix)"
say "reps=$REPS  measured G (ppoll p99, wake_granularity_2026-09-26.txt): 57.4us both Pis"

build() {
    local sha="$1" pids=() bad=0
    say ""; say "--- building $sha ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx
cd examples/perf_hil/tickle
for s in reliable_latency reliable_throughput; do
  TICKLE_CORE_BUILD=release TICKLE_RELIABLE_STATS=1 ./build.sh \$s p1 > /tmp/rba_build.log 2>&1 \
    || { echo \"BUILD FAILED \$s on \$(hostname)\"; tail -8 /tmp/rba_build.log; exit 1; }
done" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD FAILED - stopping"; exit 1; }
    local head; head=$(sh_ "$CLIENT" 'git -C ~/tickle rev-parse --short HEAD')
    say "  identity: HEAD=$head (wanted $sha)"
    [ "$head" = "$sha" ] || { say "  IDENTITY FAIL - stopping"; exit 1; }
}
# Orchestration is run_scenario.sh, not hand-rolled SSH (2026-09-26). The first version of this
# script started the server with its own -d and read its log directly; the server then exited
# before the client had finished and its RESULT line was never produced, so every rep failed the
# retry_interval_cfg_ns identity check - which lives on the SERVER line. run_scenario.sh already
# SIGINTs the server after the client returns and reads the log afterwards, for exactly that
# reason, and prints both RESULT lines.
one_run() {
    local scen="$1" out
    out=$(cd "$REPO/examples/perf_hil/tickle" && timeout 180 ./run_scenario.sh "$scen" -d 5 -Q 2>/dev/null \
        | grep '^RESULT:' | tr '\n' ' ')
    echo "$out"
}
arm() {
    local tag="$1" scen="$2" r line
    for r in $(seq 1 "$REPS"); do
        line=$(one_run "$scen")
        case "$line" in
            *core_build=release*) ;;
            *) say "$tag $scen rep$r | IDENTITY FAIL: core_build not release -> $line"; continue;;
        esac
        # reliable_latency's server RESULT carries no retry_interval_cfg_ns, so the dynamic-mode
        # assertion can only be made on the throughput scenario. Said here rather than silently
        # skipped: the latency cells are RTT evidence only, and they exercise no retries at all
        # (retransmitted=0 in every rep of the voided first run), so they cannot confirm which
        # retry mode produced them.
        case "$scen" in
            reliable_throughput_*)
                case "$line" in
                    *retry_interval_cfg_ns=0*) ;;
                    *) say "$tag $scen rep$r | IDENTITY FAIL: retry not dynamic -> $line"; continue;;
                esac;;
        esac
        say "$tag $scen rep$r | $(grep -oE '(sent|recv|send_mbps|rtt_avg_ms|rtt_max_ms|retransmitted|gap_abandoned|wire_bytes_per_sample|recovery_srtt_ns|recovery_rttvar_ns)=[0-9.]+' <<<"$line" | tr '\n' ' ')"
    done
}

for sha in "$BEFORE" "$AFTER"; do
    build "$sha"
    tag=$([ "$sha" = "$BEFORE" ] && echo BEFORE || echo AFTER)
    say ""; say "### ARM 1 unshaped ($tag)"
    set_delay 0
    arm "$tag" reliable_throughput_p1
    arm "$tag" reliable_latency_p1
    say ""; say "### ARM 2 netem delay 20ms + loss 5% ($tag)"
    set_delay "20ms loss 5%"
    arm "$tag" reliable_throughput_p1
    arm "$tag" reliable_latency_p1
    set_delay 0
done
say ""; say "=== done ==="
