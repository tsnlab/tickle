#!/usr/bin/env bash
# Follow-up to fastdds_sweep_identity.sh: the two best_effort pairs, which that run did not build.
# best_effort_latency/client.cpp is the file TickLE Dev trusts least - it was derived from the
# reliable client by stripping the instruments and switching the QoS rather than rewritten, and the
# first draft of it was wrong in exactly that way (it kept the instruments and RELIABLE). It has
# never been executed. The question here is "does it run and print a sane RESULT line", so n=1 per
# arm is enough and there is no timing arm and no A-B-A.
#   best_effort_latency  (-i 0.1 -d 10): sent should equal round(d/i)=100 in BOTH arms, recv ~ sent
#     on a clean link, and rtt close to reliable_latency's.
#   best_effort_throughput (-d 5): the server's recv and loss_pct on a clean link.
#
# Did the FastDDS lint sweep change behaviour? 18 files were refactored for clang-tidy across 9
# scenario pairs; the claim is that every change is behaviour-preserving. That claim is a reading of
# the diff, and this is the measurement.
#
# pre = 4baa3b76 (last before the sweep)   post = 44a4f59c (last batch)
#
# DESIGN, written before running:
#   Exact fields at n=1. These are deterministic per scenario (TickLE Dev's list), so one run each is
#   the right cost and any difference is a real behavioural change:
#     deadline    offered/requested_missed_total       history_depth  recv/lost
#     durability  received                             lifespan       recv/lost
#     liveliness  loss_detected
#   Timing fields at 3 reps a side. rtt/mbps/cpu vary run to run, so at n=1 there is no spread for
#   "inside normal variation" to mean anything - today c1 throughput moved 0.4% between repetitions
#   of an IDENTICAL binary, and a 2% refactor cost would hide inside a single-run difference.
#   A-B-A rather than all-pre-then-all-post. For a refactor the expected answer is "no difference", so
#   drift in rig conditions manufactures findings rather than hiding them. The second pre arm on the
#   timing scenarios is the drift check: if pre#2 differs from pre#1 outside its spread, the session
#   drifted and the pre/post comparison is void rather than negative.
#   Binaries are REMOVED before each build and checked at the path run_scenario.sh executes. Not
#   build.sh's exit status: a failed step inside its loop, or a stale binary from an earlier session,
#   would otherwise stand in for the arm. (The first version of this preflight did `rm -rf <scenario>`
#   and deleted the SOURCES, which build.sh correctly reported as a missing scenario dir.)
#   DELIBERATELY UNCHANGED by the sweep, not to be reported as regressions (TickLE Dev): the
#   liveliness client installs no SIGINT handler, and the liveliness server's listener globals are
#   non-atomic - a latent race inherited from the original.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PRE=4baa3b76; POST=44a4f59c
# lifespan_expiry joins them: the main run showed recv 95 -> 96 at n=1, and one sample out of
# a hundred cannot be told from run-to-run variation at n=1. Five reps per arm can.
EXACT="best_effort_latency best_effort_throughput"
REPEATED="lifespan_expiry"
REPS_EXACT=5
TIMING=""
ALL="$EXACT $TIMING $REPEATED"
OUT=${OUT:-/tmp/fdds_be_identity.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
srv_pids() {
    # shellcheck disable=SC2016  # expands on the rig
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */perf_hil/fastdds/*/server) echo "$p";; esac; done'
}
kill_srv() { local p; p=$(srv_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }
trap kill_srv EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== FastDDS sweep identity, $(date -Is) ==="
say "pre=$PRE  post=$POST"

build() {
    local sha="$1" pids=() bad=0
    say ""; say "--- building $sha on both rpis ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -u
cd ~/tickle && git fetch -q origin && git reset -q --hard origin/main && git clean -fdqx && git checkout -q $sha
cd examples/perf_hil/fastdds
for s in $ALL; do rm -f \"\$s/client\" \"\$s/server\"; done
miss=
for s in $ALL; do
  ./build.sh \"\$s\" > /tmp/fid_\$s.log 2>&1
  { [ -x \"\$s/client\" ] && [ -x \"\$s/server\" ]; } || miss=\"\$miss \$s\"
done
if [ -n \"\$miss\" ]; then echo \"MISSING on \$(hostname):\$miss\"; exit 1; fi
echo \"\$(hostname) built all at \$(git rev-parse --short HEAD)\"" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD/PATH CHECK FAILED - stopping"; exit 1; }
    say "  both rpis: all $(wc -w <<<"$ALL") scenarios present at the executed paths"
}
run1() {   # $1 scenario  $2 args
    kill_srv; [ -z "$(srv_pids)" ] || { echo "ABORT-server"; return; }
    local d="tickle/examples/perf_hil/fastdds/$1"
    # Taken verbatim from fastdds/run_scenario.sh:19,21 rather than guessed. An earlier draft used
    # CycloneDDS's aarch64 lib path and an invented profile name; FastDDS would then have fallen back
    # to its defaults and used every interface instead of eth0 only - a QoS change inside the very
    # comparison meant to detect QoS changes.
    local env="export LD_LIBRARY_PATH=/opt/ros/jazzy/lib; export FASTRTPS_DEFAULT_PROFILES_FILE=/home/ci/tickle/examples/perf_hil/fastdds/fastdds_eth0_only.xml;"
    sh_ "$SERVER" "$env cd ~/$d; nohup taskset -c 1-3 ./server $2 > /tmp/fid_srv.log 2>&1 < /dev/null &"
    sleep 3
    [ "$(srv_pids | wc -l)" = 1 ] || { echo "ABORT-nserver"; return; }
    local o; o=$(sh_ "$CLIENT" "$env cd ~/$d && taskset -c 1-3 ./client $2" 2>&1 | grep -m1 '^RESULT:')
    kill_srv
    # The SERVER's RESULT line carries half the fields this comparison is about -
    # requested_missed_total, recv/lost, loss_detected - and it is printed to its log on SIGINT, not
    # to the client's stdout. run_scenario.sh reads it back the same way; an earlier version of this
    # script did not, and would have compared only the client half while claiming to check the list.
    local sv; sv=$(sh_ "$SERVER" "grep -m1 '^RESULT:' /tmp/fid_srv.log 2>/dev/null" || true)
    echo "${o:-NO-CLIENT-RESULT} ${sv:-NO-SERVER-RESULT}"
}
arm() {
    local tag="$1" s r
    say ""
    for s in $EXACT; do
        local a="-d 5"; [ "$s" = best_effort_latency ] && a="-i 0.1 -d 10"
        say "$tag ONCE $s | $(run1 "$s" "$a")"
    done
    for s in $REPEATED; do
        for r in $(seq 1 "$REPS_EXACT"); do
            say "$tag REP  $s rep$r | $(run1 "$s" "-d 5" | grep -oE '(role|sent|recv|lost|loss_pct)=[a-z0-9.-]+' | tr '\n' ' ')"
        done
    done
}

build "$PRE";  arm "PRE1 "
build "$POST"; arm "POST "
say ""; say "=== done ==="
