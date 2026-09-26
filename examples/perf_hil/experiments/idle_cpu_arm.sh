#!/usr/bin/env bash
# What does an IDLE node cost? Every CPU figure in COMPARISON.MD is per-sample under load, and the
# user's instruction, translated: "use as little CPU time as possible". For a 10Base-T1S device
# that is mostly waiting, the idle cost is the one that matters, and nothing has measured it.
#
# The poll change should have transformed TickLE's: 38,508 ppoll calls in a 5 s latency run became 34,
# and an idle node now wakes ~2/s for node_update and check_liveliness instead of 10,000/s.
#
# METHOD: start a subscriber/server, send NOTHING, and sample its CPU from /proc/PID/stat over a
# fixed window. utime+stime ticks over wall time is the duty cycle. Also counts voluntary context
# switches from /proc/PID/status, which is the wake count by another name and does not need strace.
#
# HOW TO READ IT, written before running:
#   duty cycle = (utime+stime) / wall. A node that truly sleeps between events reads near 0.
#   TickLE is expected near 0 now; the same measurement on the pre-poll-change build is the control
#   that says whether the change is what did it, and should read around 10,000 wakes/s worth of work.
#   nonvoluntary_ctxt_switches rising instead of voluntary would mean it is being preempted while
#   running rather than sleeping - a different thing, and it would invalidate the duty-cycle reading.
#   If any framework's duty cycle is indistinguishable from the others', idle cost is not a
#   differentiator and this arm says so rather than being quietly dropped.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; SRV=10.1.1.213
WINDOW=${WINDOW:-20}
OUT=${OUT:-/tmp/idle_cpu.txt}
NEW=$(git -C "$(dirname "${BASH_SOURCE[0]}")/../../.." rev-parse --short origin/main); OLD=9a230a1b
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$SRV" "$@"; }
kill_all() {
  # shellcheck disable=SC2016
  sh_ 'for p in $(ls /proc|grep -E "^[0-9]+$"); do e=$(readlink /proc/$p/exe 2>/dev/null); case "$e" in */perf_hil/*/server) kill -INT "$p" 2>/dev/null;; esac; done' >/dev/null 2>&1 || true
  sleep 2
}
trap kill_all EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== idle CPU, $(date -Is), window ${WINDOW}s ==="
build() {
    local sha="$1" recipe
    [ "$sha" = "$OLD" ] && recipe="git reset -q --hard origin/main && git clean -fdqx -e install -e build -e log && git checkout -q $NEW && git checkout -q $OLD -- src/tickle.c src/hal_linux.c include/tickle/config.h include/tickle/tickle.h" \
                        || recipe="git reset -q --hard $sha && git clean -fdqx -e install -e build -e log"
    say ""; say "--- building for arm $sha ---"
    sh_ "set -e
cd ~/tickle && git fetch -q origin && $recipe
cd examples/perf_hil
for fw in tickle cyclonedds fastdds; do (cd \$fw && ./build.sh reliable_latency p1 > /tmp/idle_\$fw.log 2>&1) || { echo \"BUILD FAILED \$fw\"; tail -5 /tmp/idle_\$fw.log; exit 1; }; done
echo \"built; until_next_event in core: \$(grep -c until_next_event src/tickle.c || true)\"" | tee -a "$OUT"
}
measure() {   # $1 fw  $2 label
    local fw="$1"
    local d="tickle/examples/perf_hil/$fw/reliable_latency_p1"
    local env=""
    case "$fw" in
      cyclonedds) env="export LD_LIBRARY_PATH=/opt/ros/jazzy/lib/aarch64-linux-gnu; export CYCLONEDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name=\"eth0\"/></Interfaces></General></Domain></CycloneDDS>';";;
      fastdds)    env="export LD_LIBRARY_PATH=/opt/ros/jazzy/lib; export FASTRTPS_DEFAULT_PROFILES_FILE=/home/ci/tickle/examples/perf_hil/fastdds/fastdds_eth0_only.xml;";;
    esac
    kill_all
    local r
    r=$(sh_ "$env cd ~/$d && nohup taskset -c 1-3 ./server -i 0.1 -d 600 > /tmp/idle_srv.log 2>&1 < /dev/null & sleep 4
p=\$(for q in \$(ls /proc|grep -E "^[0-9]+\$"); do e=\$(readlink /proc/\$q/exe 2>/dev/null); case \$e in */perf_hil/$fw/*/server) echo \$q;; esac; done | head -1)
[ -n \"\$p\" ] || { echo 'NO-PID'; exit 0; }
read -r u1 s1 <<<\"\$(awk '{print \$14, \$15}' /proc/\$p/stat)\"
v1=\$(awk '/voluntary_ctxt_switches/{print \$2}' /proc/\$p/status | head -1)
n1=\$(awk '/nonvoluntary_ctxt_switches/{print \$2}' /proc/\$p/status | head -1)
sleep $WINDOW
read -r u2 s2 <<<\"\$(awk '{print \$14, \$15}' /proc/\$p/stat 2>/dev/null || echo '0 0')\"
v2=\$(awk '/voluntary_ctxt_switches/{print \$2}' /proc/\$p/status 2>/dev/null | head -1)
n2=\$(awk '/nonvoluntary_ctxt_switches/{print \$2}' /proc/\$p/status 2>/dev/null | head -1)
hz=\$(getconf CLK_TCK)
echo \"ticks=\$(( (u2-u1)+(s2-s1) )) hz=\$hz vol=\$(( v2-v1 )) nonvol=\$(( n2-n1 ))\"
kill -INT \"\$p\" 2>/dev/null || true")
    kill_all
    local ticks hz vol nonvol
    ticks=$(grep -oE 'ticks=[0-9-]+' <<<"$r" | cut -d= -f2); hz=$(grep -oE 'hz=[0-9]+' <<<"$r" | cut -d= -f2)
    vol=$(grep -oE 'vol=[0-9-]+' <<<"$r" | head -1 | cut -d= -f2); nonvol=$(grep -oE 'nonvol=[0-9-]+' <<<"$r" | cut -d= -f2)
    if [ -z "${ticks:-}" ] || [ -z "${hz:-}" ]; then say "$2 $fw | MEASURE FAILED: $r"; return; fi
    say "$2 $fw | CPU ${ticks} ticks / ${WINDOW}s @${hz}Hz = $(python3 -c "print(f'{$ticks/$hz/$WINDOW*100:.3f}%')") duty | vol_ctxt=$vol ($(python3 -c "print(f'{$vol/$WINDOW:.1f}')")/s) nonvol=$nonvol"
}
build "$NEW"; for fw in tickle cyclonedds fastdds; do measure "$fw" "NEW "; done
build "$OLD"; measure tickle "OLD "
say ""; say "=== done ==="
