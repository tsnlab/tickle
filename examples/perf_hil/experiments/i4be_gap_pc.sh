#!/usr/bin/env bash
# i4be_gap_pc.sh - the I4 cell (perf_test Array4m, 15 Hz, cross-"host") on the PC, through rmw: two private network
# namespaces joined by a veth named eth0 in each (192.168.10.2 publisher, .3 subscriber), the publisher's egress shaped
# to 1 Gbit/s by tbf (the rig's link). perf_test from a pc_preflight_build.sh build, publisher alone in one namespace and
# subscriber alone in the other, as rmw_keepall_rig.sh runs them. Every perf_test runs under `perf stat`: task-clock
# (CPU ms), cycles and instructions, user and kernel - per sample delivered (subscriber) and per sample sent (publisher).
#
# Why (2026-10-15, ab/i4be-gap): on the rig (RESULTS.md L4, main f5fcda06) rmw_tickle lost I4 best effort on CPU to
# Fast DDS as shipped (sub 2.19 vs 2.10 ms/MB, pub 3.72 vs 3.61) and drew I4 RELIABLE. The runs' rusage columns show
# rmw_tickle's best effort costing what its RELIABLE costs, and Fast DDS's best effort ~4% below its RELIABLE; the gap
# is rmw_tickle's common path, and it is kernel time (sub stime 2.94 vs 2.29 s per 60 s run, utime 5.47 vs 5.81).
# This harness measures each rmw_tickle build's subscriber and publisher cost per sample in both QoS, with Fast DDS
# beside it as the control no rmw_tickle change can touch. NOT a rig figure and never to be tabled as one: x86, veth
# (the receiver's softirq work runs in the sender's context), no IRQ-CPU placement like the rig's.
#
# Arms: ARMS="<name>:<qos> ..." with qos be (BEST_EFFORT KEEP_LAST 5) or rel (RELIABLE KEEP_LAST 10); name is fdds
# (rmw_fastrtps_cpp with fastdds/fastdds_eth0_only.xml) or a rmw_tickle build named by TICKLE_<name>=<install dir>
# (default: TICKLE_A = $PF/install), with TENV_<name>="VAR=value ..." exported at both ends (a runtime switch: two arms
# of one build). Arms rotate per rep. Every rmw_tickle subscriber runs with the rig Pis' classic
# drain buffers (tests/support/refuse_pbuf_ring.so, TT_REFUSE_PBUF_RING=all: the Pis' kernel refuses the pbuf ring).
# Every perf_test of every arm runs under sockbuf_clamp.so: socket buffers as the rig's 212992 rmem_max/wmem_max grant
# them (the PC's limits are 4 MB and global). Fast DDS runs with FASTDDS_BUILTIN_TRANSPORTS=UDPv4 (no shared memory
# between the namespaces, which share /dev/shm; the rig's eth0 XML profile did not discover on this PC's Fast DDS).
#
# HOW TO READ IT - written before the first run, enforced by i4be_gap_pc_read.py:
#   VOID     a run whose perf_test maps do not show the arm's own rmw library, with no perf stat counts, or that
#            delivered below 98% of what was sent after the first second.
#   METRIC   subscriber task-clock ms per sample delivered (PRIMARY) and publisher task-clock per sample sent; user and
#            kernel cycles per sample beside them.
#   FLOOR    per metric, two arms compared by mean difference against 2 x its standard error (reps are interleaved).
#            fdds:be against fdds:rel is the control: it shows Fast DDS's best-effort saving on this PC. Between two
#            rmw_tickle builds of one QoS, BETTER / WORSE only beyond 2 SE AND beyond the spread of the fdds arms'
#            own rep-to-rep difference; else HELD.
#
# Usage: i4be_gap_pc.sh   env: REPS(4) DUR(20) ARMS PF OUT(~/largemsg_pc/i4be_gap/results.txt) PROFILE(0; 1 = one
#        perf record -g of every arm's subscriber instead of perf stat, to $WORK/prof/). Needs passwordless
#        `sudo ip`; tc runs through `ip netns exec`. Detached (CLAUDE.md "Long-running work"). PC only: every process is
#        in a private namespace, under ulimit -v and a timeout. The last line is always "i4be_gap_pc done rc=<n>".
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
REPS=${REPS:-4}
DUR=${DUR:-20}
PF=${PF:-$HOME/pc_preflight_builds/f5fcda06281c}
TICKLE_A=${TICKLE_A:-$PF/install}
ARMS=${ARMS:-"A:be A:rel fdds:be fdds:rel"}
WORK=${WORK:-$HOME/largemsg_pc/i4be_gap}
OUT=${OUT:-$WORK/results.txt}
PROFILE=${PROFILE:-0}
VMEM_KB=${VMEM_KB:-2097152}
ROS_SETUP=${ROS_SETUP:-/opt/ros/lyrical/setup.bash}
PT=$PF/perf/install/performance_test/lib/performance_test/perf_test
NSP=i4p$$
NSS=i4s$$
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns pids "$NSP" 2>/dev/null | while read -r p; do kill -INT "$p" 2>/dev/null; done
    sudo -n ip netns pids "$NSS" 2>/dev/null | while read -r p; do kill -INT "$p" 2>/dev/null; done
    sleep 1
    sudo -n ip netns del "$NSP" 2>/dev/null
    sudo -n ip netns del "$NSS" 2>/dev/null
    echo "i4be_gap_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK" "$TMPDIR" "$WORK/prof"
[ -x "$PT" ] || { echo "no perf_test at $PT"; exit 1; }
cc -shared -fPIC -O2 -o "$WORK/refuse_pbuf_ring.so" "$REPO/tests/support/refuse_pbuf_ring.c" -ldl || exit 1
cc -shared -fPIC -O2 -o "$WORK/sockbuf_clamp.so" "$HERE/sockbuf_clamp.c" || exit 1

sudo -n ip netns add "$NSP" && sudo -n ip netns add "$NSS" || exit 1
sudo -n ip link add "i4v$$p" type veth peer name "i4v$$s" || exit 1
for ns in "$NSP" "$NSS"; do
    case $ns in "$NSP") dev=i4v$$p ip=192.168.10.2 ;; *) dev=i4v$$s ip=192.168.10.3 ;; esac
    sudo -n ip link set "$dev" netns "$ns" && sudo -n ip -n "$ns" link set "$dev" name eth0 &&
        sudo -n ip -n "$ns" addr add "$ip/24" brd 192.168.10.255 dev eth0 && sudo -n ip -n "$ns" link set lo up &&
        sudo -n ip -n "$ns" link set eth0 up && sudo -n ip -n "$ns" route add default dev eth0 || exit 1
done
sudo -n ip netns exec "$NSP" tc qdisc add dev eth0 root tbf rate 1gbit burst 64kb latency 100ms || exit 1
echo "=== i4be_gap_pc $(date -Is) reps=$REPS dur=${DUR}s arms='$ARMS' pf=$PF profile=$PROFILE load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"
for a in $ARMS; do
    n=${a%%:*}
    [ "$n" = fdds ] && continue
    v=TICKLE_$n
    [ -n "${!v:-}" ] || { echo "arm $a: TICKLE_$n unset"; exit 1; }
    e=TENV_$n
    echo "ARM $n install=${!v} env='${!e:-}' lib_sha256=$(sha256sum "${!v}/rmw_tickle/lib/librmw_tickle.so" | cut -c1-16)" | tee -a "$OUT"
done

envfile() { # <name> <role>: the environment a perf_test of this arm starts with
    local n=$1
    echo "source $ROS_SETUP"
    echo "unset FASTRTPS_DEFAULT_PROFILES_FILE FASTDDS_DEFAULT_PROFILES_FILE RMW_FASTRTPS_USE_QOS_FROM_XML TT_UDP_OFFLOAD TT_UDP_CSUM0"
    if [ "$n" = fdds ]; then
        echo "export RMW_IMPLEMENTATION=rmw_fastrtps_cpp FASTDDS_BUILTIN_TRANSPORTS=UDPv4 LD_PRELOAD=$WORK/sockbuf_clamp.so"
    else
        local v=TICKLE_$n
        echo "source ${!v}/local_setup.bash"
        echo "export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255"
        local e=TENV_$n
        [ -n "${!e:-}" ] && echo "export ${!e}"
        if [ "$2" = sub ]; then echo "export TT_REFUSE_PBUF_RING=all LD_PRELOAD=$WORK/refuse_pbuf_ring.so:$WORK/sockbuf_clamp.so"
        else echo "export LD_PRELOAD=$WORK/sockbuf_clamp.so"; fi
    fi
    echo "source $PF/perf/install/local_setup.bash"
}
inns() { # <ns> <cmd>: as this user, in the namespace
    sudo -n ip netns exec "$1" sudo -n -u "$USER" bash -c "$2"
}
sum_col() { # <log> <column name>: the column summed over perf_test's data rows
    awk -v want="$2" -F',' '/^T_experiment/ { for (i = 1; i <= NF; i++) { g = $i; gsub(/[ \t]/, "", g); if (g == want) c = i } next }
        c && /^[0-9]+\.[0-9]+,/ { v = $c; gsub(/[ \t]/, "", v); s += v } END { print s + 0 }' "$1"
}
last_row() { grep -E '^[0-9]+\.[0-9]+,' "$1" | tail -1 | tr -d '\t ' | awk -F, '{print "utime="$20" stime="$21" nvcsw="$34" nivcsw="$35}'; }
pstat() { # <perf stat file> <event>
    grep -m1 ",$2," "$1" 2>/dev/null | cut -d, -f1
}

run() { # <arm> <rep>
    local arm=$1 rep=$2 n=${1%%:*} q=${1##*:} qos stem
    case $q in be) qos="--keep_last --history_depth 5" ;; rel) qos="--reliable --keep_last --history_depth 10" ;; esac
    stem=$WORK/run_${n}_${q}
    envfile "$n" sub > "$stem.sub.env"
    envfile "$n" pub > "$stem.pub.env"
    local common="-c ROS2 -t Array4m $qos --dds_domain_id 77" ros="--ros-args --param start_type_description_service:=false"
    local pre_s pre_p
    if [ "$PROFILE" = 1 ]; then
        # The subscriber only (two recordings at once exceed the PC's perf_event_mlock_kb), at the PC's 2 kHz limit.
        pre_s="perf record -F 1999 -g -o $WORK/prof/${n}_${q}_sub.data --"
        pre_p="perf stat -x, -e task-clock -o $stem.pub.perf --"
    else
        pre_s="perf stat -x, -e task-clock,cycles:u,cycles:k,instructions:u,instructions:k -o $stem.sub.perf --"
        pre_p="perf stat -x, -e task-clock,cycles:u,cycles:k,instructions:u,instructions:k -o $stem.pub.perf --"
    fi
    rm -f "$stem.sub.perf" "$stem.pub.perf" "$stem.sub.maps" "$stem.pub.maps" "$stem.sub.threads"
    inns "$NSS" "source $stem.sub.env; ulimit -v $VMEM_KB; exec timeout $((DUR + 30)) $pre_s $PT $common -p 0 -s 1 --expected_num_pubs 1 --max_runtime $((DUR + 8)) $ros" > "$stem.sub.log" 2>&1 &
    local sp=$!
    sleep 2
    inns "$NSP" "source $stem.pub.env; ulimit -v $VMEM_KB; exec timeout $((DUR + 30)) $pre_p $PT $common -r 15 -p 1 -s 0 --expected_num_subs 1 --max_runtime $DUR $ros" > "$stem.pub.log" 2>&1 &
    local pp=$!
    # The rmw library each perf_test loaded, from its own maps (perf_test is perf's child; matched by /proc exe).
    sleep 4
    local p c
    for p in $(sudo -n ip netns pids "$NSS") $(sudo -n ip netns pids "$NSP"); do
        [ "$(readlink "/proc/$p/exe" 2>/dev/null)" = "$(readlink -f "$PT")" ] || continue
        c=$(grep -o '/[^ ]*librmw_[a-z_]*\.so' "/proc/$p/maps" 2>/dev/null | sort -u | tr '\n' ' ')
        if sudo -n ip netns identify "$p" 2>/dev/null | grep -qx "$NSS"; then
            echo "$c" > "$stem.sub.maps"
            # The subscriber's threads: perf_test's main thread (tid = pid) spins on its stats lock once a second for as
            # long as the executor thread holds it (performance_test SpinLock, DataRunner::sync_reset()), a cost of
            # perf_test that depends on when the rmw's wait returns; every other thread is the rmw's and the
            # executor's work. Each thread's utime and stime (clock ticks) every 0.5 s while it lives; the last
            # snapshot is read.
            ( while [ -d "/proc/$p/task" ]; do
                  snap=$(for t in /proc/"$p"/task/*; do read -r -a st < "$t/stat" 2>/dev/null && echo "${t##*/} ${st[13]} ${st[14]}"; done)
                  [ -n "$snap" ] && echo "$snap" > "$stem.sub.threads.tmp" && mv "$stem.sub.threads.tmp" "$stem.sub.threads"
                  sleep 0.5
              done; echo "pid $p" >> "$stem.sub.threads" ) &
        else echo "$c" > "$stem.pub.maps"; fi
    done
    wait "$pp"
    wait "$sp"
    sleep 1
    local recv sent want ok=1 why=""
    recv=$(sum_col "$stem.sub.log" received)
    sent=$(sum_col "$stem.pub.log" sent)
    if [ "$n" = fdds ]; then want=librmw_fastrtps_cpp.so; else v=TICKLE_$n; want="${!v}/rmw_tickle/lib/librmw_tickle.so"; fi
    for r in sub pub; do grep -q -- "$want" "$stem.$r.maps" 2>/dev/null || { ok=0; why="$why $r-maps($(cat "$stem.$r.maps" 2>/dev/null))"; }; done
    cp "$stem.sub.log" "$WORK/logs_${n}_${q}_r${rep}.sub.log" 2>/dev/null
    cp "$stem.pub.log" "$WORK/logs_${n}_${q}_r${rep}.pub.log" 2>/dev/null
    if [ "$PROFILE" = 1 ]; then
        echo "PROF arm=$arm rep=$rep sent=$sent recv=$recv ok=$ok$why" | tee -a "$OUT"
        return
    fi
    local f vals=""
    for r in sub pub; do
        for f in task-clock cycles:u cycles:k instructions:u instructions:k; do
            vals="$vals ${r}_${f//:/_}=$(pstat "$stem.$r.perf" "$f")"
        done
        vals="$vals ${r}_$(last_row "$stem.$r.log" | sed "s/ / ${r}_/g")"
    done
    # sub_main_ticks: perf_test's main thread (its spin); sub_work_ticks: every other thread (clock ticks, 100 a second)
    vals="$vals $(awk '/^pid / { pid = $2; next } { t[$1] = $2 + $3 } END { for (k in t) { if (k == pid) m += t[k]; else w += t[k] } if (pid == "") print "sub_threads=none"; else print "sub_main_ticks=" m + 0 " sub_work_ticks=" w + 0 }' "$stem.sub.threads" 2>/dev/null || echo sub_threads=none)"
    local cnt
    cnt=$(grep -h -o -E 'Node [0-9]+ (rx|large|hybrid): .*' "$stem.sub.log" "$stem.pub.log" | tr ' ' '\n' |
        grep -E '^(direct_reads|direct_runs|prefetch|direct_prefetches|empty_reads|timer_sets|timer_waits|send_waits|runs|drain_datagrams|socket_drops)=' | tr '\n' ' ')
    echo "RUN arm=$arm rep=$rep ok=$ok$why sent=$sent recv=$recv$vals | $cnt" | tee -a "$OUT"
}

read -r -a arm_list <<<"$ARMS"
nA=${#arm_list[@]}
for rep in $(seq 1 "$REPS"); do
    for i in $(seq 0 $((nA - 1))); do
        run "${arm_list[$(((i + rep - 1) % nA))]}" "$rep"
    done
done
echo "=== done $(date -Is) load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"
if [ "$PROFILE" = 1 ]; then rc=0; exit 0; fi
python3 "$HERE/i4be_gap_pc_read.py" "$OUT" | tee -a "$OUT.read"
rc=${PIPESTATUS[0]}
