#!/bin/bash
# rx_window.sh MODE DELAY WIN FREQ - the subscriber's measurement window for rmw_keepall_rig.sh RX_PROFILE (rx_perf_read.py
# reads its output). Runs ON the subscriber's host (a Pi, or the PC preflight's namespace), copied there as
# /tmp/ka_rxwin.sh. The subscriber is perf_test's own PID from /tmp/ka_sub.pid (written by the launcher, never searched for
# by a pattern), accepted only while /proc/<pid>/exe is a perf_test.
#
# DELAY s after it is started (right after the publisher's launch) - past start-up and discovery - it prints one SNAP t0
# block, then for WIN s:
#   plain   nothing (sleep)                               stat    perf stat --per-thread -x, -p <pid> -o /tmp/ka_rx.stat
#   record  perf record -g -F FREQ -e cycles -p <pid>     strace  strace -f -c -p <pid> (attach; needs ptrace_scope 0)
#   strace_wrap  nothing here: the subscriber was launched under strace -f -c (whole life; ptrace_scope != 0)
# then SNAP t1, so the instrument and the window's counters cover the same wall interval. A SNAP block is one SNAP line
# (wall clock, the process's summed task run time from schedstat in ns, voluntary/involuntary context switches, utime/stime,
# the host's /proc/stat cpu line, NET_RX softirqs, eth0 rx bytes, boot id) and one TASK line per thread.
# The last line is always "RXWIN mode=... rc=... alive=..." or "RXWIN_VOID ...".
mode=$1 delay=$2 win=$3 freq=$4
EVENTS=instructions:u,instructions:k,cycles:u,cycles:k,task-clock,context-switches,cpu-migrations,page-faults
sleep "$delay"
p=$(cat /tmp/ka_sub.pid 2>/dev/null)
case "$(readlink "/proc/$p/exe" 2>/dev/null)" in
*/perf_test) ;;
*)
    echo "RXWIN_VOID no perf_test at pid '$p' after ${delay}s"
    exit 0
    ;;
esac
snap() { # $1 label
    local t rt v n tot_rt=0 tot_v=0 tot_n=0 k=0 comm st cpu
    for t in /proc/"$p"/task/[0-9]*; do
        read -r rt _ <"$t/schedstat" 2>/dev/null || rt=0
        v=$(awk '/^voluntary_ctxt_switches/ {print $2}' "$t/status" 2>/dev/null)
        n=$(awk '/^nonvoluntary_ctxt_switches/ {print $2}' "$t/status" 2>/dev/null)
        comm=$(tr ' ' '_' <"$t/comm" 2>/dev/null)
        echo "TASK $1 tid=${t##*/} comm=${comm:-?} rt_ns=${rt:-0} vol=${v:-0} nvol=${n:-0}"
        tot_rt=$((tot_rt + ${rt:-0}))
        tot_v=$((tot_v + ${v:-0}))
        tot_n=$((tot_n + ${n:-0}))
        k=$((k + 1))
    done
    # utime and stime are fields 14 and 15 of /proc/<pid>/stat, counted after the ")" that ends the command name.
    st=$(sed 's/.*) //' "/proc/$p/stat" 2>/dev/null | awk '{print "utime=" $12 " stime=" $13}')
    cpu=$(awk '/^cpu / {printf "cpu_user=%s cpu_nice=%s cpu_system=%s cpu_idle=%s cpu_iowait=%s cpu_irq=%s cpu_softirq=%s cpu_steal=%s", $2, $3, $4, $5, $6, $7, $8, $9}' /proc/stat)
    echo "SNAP $1 wall_ns=$(date +%s%N) task_ns=$tot_rt vol=$tot_v nvol=$tot_n threads=$k ${st:-utime=? stime=?} $cpu" \
        "clk_tck=$(getconf CLK_TCK) ncpu=$(nproc)" \
        "net_rx_softirqs=$(awk '/NET_RX/ {s = 0; for (i = 2; i <= NF; i++) s += $i; print s}' /proc/softirqs)" \
        "eth0_rx_bytes=$(cat /sys/class/net/eth0/statistics/rx_bytes 2>/dev/null)" \
        "boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
}
rm -f /tmp/ka_rx.stat /tmp/ka_rx.data /tmp/ka_rx.data.old /tmp/ka_rx.strace /tmp/ka_rx.err
snap t0
rc=0
case "$mode" in
plain | strace_wrap) sleep "$win" ;;
stat) perf stat --per-thread -x, -o /tmp/ka_rx.stat -e "$EVENTS" -p "$p" -- sleep "$win" 2>/tmp/ka_rx.err || rc=$? ;;
record) perf record -q -g -F "$freq" -e cycles -o /tmp/ka_rx.data -p "$p" -- sleep "$win" 2>/tmp/ka_rx.err || rc=$? ;;
strace)
    timeout -s INT "$win" strace -f -c -o /tmp/ka_rx.strace -p "$p" 2>/tmp/ka_rx.err
    rc=$?
    [ "$rc" = 124 ] && rc=0 # the timeout is how the window ends
    ;;
*)
    echo "RXWIN_VOID unknown mode '$mode'"
    exit 0
    ;;
esac
snap t1
alive=0
case "$(readlink "/proc/$p/exe" 2>/dev/null)" in */perf_test) alive=1 ;; esac
echo "RXWIN mode=$mode rc=$rc alive=$alive pid=$p delay=$delay win=$win freq=$freq err=[$(head -c 300 /tmp/ka_rx.err 2>/dev/null | tr '\n' ' ')]"
