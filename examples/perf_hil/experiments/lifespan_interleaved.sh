#!/usr/bin/env bash
# lifespan_expiry, PRE and POST interleaved rep by rep.
#
# Why interleaving rather than more blocked reps (TickLE Dev): blocked arms turn any slow drift in
# the session - thermal, discovery-cache state on the rpis, the other host's load - into an arm
# difference, and the effect in question is 0.8 samples, which at -i 0.02 is 16 ms of relative phase
# between the two hosts' 50 ms match polls. That is a small enough drift to be produced by the
# session rather than by the code. Alternating removes it: a drift now lands on both arms equally.
#
# Both binaries have to outlive the checkouts, because `git clean -fdqx -e install -e build -e log` wipes untracked files in the
# tree. So each arm is built once and its client/server copied to /tmp/lifespan_{pre,post} on both
# rpis, and every run executes from there.
#
# HOW TO READ IT, written before running:
#   The estimator is the PER-REP DIFFERENCE, post_lost[i] - pre_lost[i], not the difference of means.
#   That is the whole point of pairing: a drift that moves both arms cancels inside each pair.
#   real if |mean(diff)| > 2 x SE(diff), otherwise not resolved.
#   A spread of the paired differences much wider than the arms' own sd would mean the pairing is not
#   pairing anything - the two runs in a pair are far enough apart in time to drift between them -
#   and then neither reading is safe.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PRE=4baa3b76; POST=44a4f59c; REPS=${REPS:-15}; SCEN=lifespan_expiry
OUT=${OUT:-/tmp/lifespan_inter.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
srv_pids() {
    # shellcheck disable=SC2016  # expands on the rig
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in /tmp/lifespan_*/server) echo "$p";; esac; done'
}
kill_srv() { local p; p=$(srv_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }
trap kill_srv EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== lifespan interleaved, $(date -Is) ==="
stage() {   # $1 sha  $2 dest
    local sha="$1" dest="$2" pids=() bad=0
    say "--- staging $sha -> $dest ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -u
cd ~/tickle && git fetch -q origin && git reset -q --hard origin/main && git clean -fdqx -e install -e build -e log && git checkout -q $sha
cd examples/perf_hil/fastdds && rm -f $SCEN/client $SCEN/server
./build.sh $SCEN > /tmp/ls_build.log 2>&1
{ [ -x $SCEN/client ] && [ -x $SCEN/server ]; } || { echo MISSING; tail -5 /tmp/ls_build.log; exit 1; }
rm -rf $dest && mkdir -p $dest && cp $SCEN/client $SCEN/server $dest/
[ -x $dest/client ] && [ -x $dest/server ] || { echo COPY-FAILED; exit 1; }
echo \"staged \$(git rev-parse --short HEAD) on \$(hostname)\"" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "STAGE FAILED for $sha"; exit 1; }
}
run_from() {   # $1 dir
    kill_srv; [ -z "$(srv_pids)" ] || { echo "ABORT-server"; return; }
    local env="export LD_LIBRARY_PATH=/opt/ros/jazzy/lib; export FASTRTPS_DEFAULT_PROFILES_FILE=/home/ci/tickle/examples/perf_hil/fastdds/fastdds_eth0_only.xml;"
    sh_ "$SERVER" "$env cd $1; nohup taskset -c 1-3 ./server -d 5 > /tmp/ls_srv.log 2>&1 < /dev/null &"
    sleep 3
    [ "$(srv_pids | wc -l)" = 1 ] || { echo "ABORT-nserver"; return; }
    sh_ "$CLIENT" "$env cd $1 && taskset -c 1-3 ./client -d 5" >/dev/null 2>&1
    kill_srv
    sh_ "$SERVER" "grep -m1 '^RESULT:' /tmp/ls_srv.log 2>/dev/null" || echo NO-RESULT
}
stage "$PRE"  /tmp/lifespan_pre
stage "$POST" /tmp/lifespan_post
say ""
for r in $(seq 1 "$REPS"); do
    a=$(run_from /tmp/lifespan_pre  | grep -oE 'lost=[0-9]+' | head -1 | cut -d= -f2)
    b=$(run_from /tmp/lifespan_post | grep -oE 'lost=[0-9]+' | head -1 | cut -d= -f2)
    say "pair$r | pre_lost=${a:-na} post_lost=${b:-na}"
done
say ""; say "=== done ==="
