#!/usr/bin/env bash
# Is today's TickLE faster or slower than the TickLE whose numbers are published in COMPARISON.MD?
# The user asked directly, and the comparison mixes two things that have to be separated:
#   (1) the build fix (-O0 -> -O2), which is not a change to TickLE at all, only to how we compiled it
#   (2) the night's changes (scheduler-driven poll, thread safety, dynamic retry, KEEP_ALL byte budget)
#
# A  b9fad3c1 at -O0   the campaign build, exactly as COMPARISON.MD's published figures were measured
# B  b9fad3c1 at -O2   the same TickLE, compiled properly - isolates (1)
# C  current  at -O2    today's TickLE as a user would get it, defaults and all - A->C is the net answer
#
# P1 only: its payload is 76 B in every commit, so it is the one cell that is the same experiment
# across the range. b9fad3c1's P2 is 1388 B against today's 1292 B, which is a different message.
# Interleaved, 3 reps, identity asserted where the field exists.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; C=10.1.1.214; S=10.1.1.213
OLD=b9fad3c1; NEW=$(git -C /home/semih/tickle rev-parse --short origin/main)
OUT=${OUT:-/tmp/before_after.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
pids_() {
  # shellcheck disable=SC2016
  sh_ "$S" 'for p in $(ls /proc|grep -E "^[0-9]+$"); do e=$(readlink /proc/$p/exe 2>/dev/null); case "$e" in /tmp/ba_*/server) echo "$p";; esac; done'
}
kill_() { local p; p=$(pids_); [ -n "$p" ] && { sh_ "$S" "kill -INT $p" >/dev/null 2>&1; sleep 2; }; return 0; }
trap kill_ EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== before/after TickLE, $(date -Is)  OLD=$OLD  NEW=$NEW ==="
for h in "$C" "$S"; do
  sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin
for spec in 'A $OLD debug' 'B $OLD release' 'C $NEW release'; do
  set -- \$spec; arm=\$1; sha=\$2; opt=\$3
  git reset -q --hard \$sha && git clean -fdqx -e install -e build -e log && rm -rf \$HOME/tickle_local_install*
  cd examples/perf_hil/tickle
  if [ \$opt = release ]; then BUILD_TYPE=release ./build.sh reliable_throughput p1 > /tmp/ba_\$arm.log 2>&1; else BUILD_TYPE=debug ./build.sh reliable_throughput p1 > /tmp/ba_\$arm.log 2>&1; fi
  [ -x reliable_throughput_p1/client ] || { echo \"BUILD FAILED \$arm\"; tail -5 /tmp/ba_\$arm.log; exit 1; }
  rm -rf /tmp/ba_\$arm && mkdir -p /tmp/ba_\$arm && cp reliable_throughput_p1/client reliable_throughput_p1/server /tmp/ba_\$arm/
  echo \"\$(hostname) \$arm: peek_scheduler calls=\$(objdump -d /tmp/ba_\$arm/client | grep -c 'bl.*<peek_scheduler>' || true)\"
  cd ~/tickle
done" &
done
wait
for r in 1 2 3; do
  for arm in ba_A ba_B ba_C; do
    kill_; [ -z "$(pids_)" ] || { say "$arm rep$r ABORT"; continue; }
    sh_ "$S" "cd /tmp/$arm; nohup taskset -c 1-3 ./server -d 5 -Q > /tmp/ba_srv.log 2>&1 < /dev/null &"
    sleep 3
    n=$(pids_ | wc -l); [ "$n" = 1 ] || { say "$arm rep$r ABORT-n=$n"; continue; }
    cl=$(sh_ "$C" "cd /tmp/$arm && taskset -c 1-3 ./client -d 5 -Q" | grep -m1 '^RESULT:')
    kill_
    sv=$(sh_ "$S" "grep -m1 '^RESULT:' /tmp/ba_srv.log 2>/dev/null" || true)
    say "$arm rep$r | $(grep -oE '(sent|send_mbps|cpu_s_per_Msample|peak_rss_kb)=[0-9.]+' <<<"$cl" | tr '\n' ' ')| recv=$(grep -oE 'recv=[0-9]+' <<<"$sv" | head -1 | cut -d= -f2)"
  done
done
say "=== done ==="
