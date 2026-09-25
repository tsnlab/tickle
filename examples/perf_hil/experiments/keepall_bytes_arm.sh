#!/usr/bin/env bash
# Target C: does bounding KEEP_ALL by BYTES instead of by a constant 2048 samples turn c4's memory
# loss into a win, and what does it cost? (OPTIMIZATION_PLAN.md section 9a, target C.)
#
# The model from the campaign: peak_rss_kb = C + reliable_depth x record_bytes, with C constant to
# 13 KB across four payload shapes. At P4 the touched arena is 5,651 KB of a 7,384 KB peak, and
# CycloneDDS sits at 5,608 KB - the one memory cell TickLE loses. A byte budget works not by
# reserving less but by blocking the writer sooner, so the window holds fewer unacked bytes
# (TickLE Dev's correction to an earlier, wrong description of this as an allocation problem).
#
# ONE build, two arms differing only by the harness flag -M, so nothing rests on a checkout: the
# default count bound against RMW_TICKLE_KEEP_ALL_BYTES' 512 KiB. arena_bytes= reports the arena the
# run actually used, which is the identity check.
#
# HOW TO READ IT, written before running:
#   TARGET   P4 client peak_rss_kb falls, and to beat CycloneDDS it must fall below 5,608 KB.
#            Predicted from the model: ~1,733 KB base + 512 KiB touched = ~2,250 KB.
#   COST, and this is what decides whether the trade is free:
#     - P1 and P4 send_mbps must stay inside the no-flag arm's spread. A 512 KiB budget at P4 holds
#       512*1024/2824 = 185 samples against the default 2048, an 11x narrower window, so KEEP_ALL's
#       back-pressure engages far sooner. If throughput drops outside the spread, the budget is too
#       small for this payload and the win is bought with send rate.
#     - sent == recv must still hold. This is RELIABLE: a narrower window is exactly where KEEP_ALL
#       starts dropping unacked samples on the byte bound, which is a proven path rather than a
#       theoretical one. Any shortfall means the budget breaks the guarantee, and then it is not a
#       trade at all.
#     - write_fail must stay 0. A rise means the writer is being refused rather than blocked.
#   IDENTITY  arena_bytes must match the arm: the default computed bound with no flag, 524288 with
#            -M 524288. A run reporting the other arm's arena is VOID, not a data point.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PIN="taskset -c 1-3"; REPS=${REPS:-3}; BUDGET=524288
OUT=${OUT:-/tmp/keepall_bytes.txt}
SHA=$(git -C "$(dirname "${BASH_SOURCE[0]}")/../../.." rev-parse --short origin/main)
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
srv_pids() {
    # shellcheck disable=SC2016  # expands on the rig
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */perf_hil/tickle/*/server) echo "$p";; esac; done'
}
kill_srv() { local p; p=$(srv_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }
trap kill_srv EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== KEEP_ALL byte budget, $(date -Is), repo $SHA, budget $BUDGET ==="
pids=(); bad=0
say "--- building p1 and p4 on both rpis ---"
for h in "$CLIENT" "$SERVER"; do
    sh_ "$h" "set -u
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx
cd examples/perf_hil/tickle
for s in p1 p4; do
  ./build.sh reliable_throughput \$s > /tmp/kb_build_\$s.log 2>&1
  { [ -x reliable_throughput_\$s/client ] && [ -x reliable_throughput_\$s/server ]; } \
    || { echo \"MISSING \$s on \$(hostname)\"; tail -5 /tmp/kb_build_\$s.log; exit 1; }
done
echo \"\$(hostname) ok at \$(git rev-parse --short HEAD)\"" &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p" || bad=1; done
[ "$bad" = 0 ] || { say "BUILD FAILED"; exit 1; }

run_one() {   # $1 shape  $2 extra
    kill_srv; [ -z "$(srv_pids)" ] || { echo "ABORT-server"; return; }
    local d="tickle/examples/perf_hil/tickle/reliable_throughput_$1"
    sh_ "$SERVER" "cd ~/$d; nohup $PIN ./server -d 5 -Q $2 > /tmp/kb_srv.log 2>&1 < /dev/null &"
    sleep 3
    [ "$(srv_pids | wc -l)" = 1 ] || { echo "ABORT-nserver"; return; }
    local cl; cl=$(sh_ "$CLIENT" "cd ~/$d && $PIN ./client -d 5 -Q $2" | grep -m1 '^RESULT:')
    kill_srv
    local sv; sv=$(sh_ "$SERVER" "grep -m1 '^RESULT:' /tmp/kb_srv.log 2>/dev/null" || true)
    echo "$cl || $sv"
}
arm() {   # $1 tag  $2 shape  $3 extra  $4 expected_arena(or any)
    local tag="$1" r line arena
    for r in $(seq 1 "$REPS"); do
        line=$(run_one "$2" "$3")
        arena=$(grep -oE 'arena_bytes=[0-9]+' <<<"$line" | head -1 | cut -d= -f2)
        if [ "$4" != any ] && [ "${arena:-x}" != "$4" ]; then
            say "$tag rep$r VOID(arena_bytes=${arena:-none}, wanted $4)"; continue
        fi
        say "$tag rep$r | $(grep -oE '(sent|send_mbps|peak_rss_kb|write_fail|throttle_lag|arena_bytes)=[0-9.]+' <<<"${line%%||*}" | tr '\n' ' ')| server $(grep -oE '(recv|lost)=[0-9]+' <<<"${line##*||}" | tr '\n' ' ')"
    done
}
for shape in p1 p4; do
    say ""; say "### $shape, default count bound (no -M)"
    arm "DEF $shape" "$shape" "" any
    say "### $shape, byte budget -M $BUDGET"
    arm "BUD $shape" "$shape" "-M $BUDGET" "$BUDGET"
done
say ""; say "=== done ==="
