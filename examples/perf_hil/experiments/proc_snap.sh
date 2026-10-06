#!/bin/sh
# proc_snap.sh <pid> <exe-glob> - one process's CPU so far and its memory, read from /proc by an instrument the process
# does not control. POSIX sh, so it can be streamed to a Pi and run there: ssh host "sh -s -- PID '*/server'" <proc_snap.sh
#
# Prints ONE line, always, so a caller can tell "could not look" from a reading:
#   state=ok utime_s=<s> stime_s=<s> cpu_s=<s> vmhwm_kb=<kB> vmrss_kb=<kB> rssshmem_kb=<kB>
#   state=gone           the pid is not a live process whose executable matches <exe-glob>
#
# The process is identified by /proc/<pid>/exe against the glob and never by a name or command-line pattern (CLAUDE.md:
# a pattern eventually matches the checking command itself). The pid comes from the caller's own launch.
#
# What the numbers are. utime/stime are fields 14 and 15 of /proc/<pid>/stat, which for a thread-group leader sum every
# thread of the process, live and exited, in clock ticks (getconf CLK_TCK). VmHWM is the peak resident set over the
# process's life; VmRSS and RssShmem are now. RssShmem is the part of the resident set that is shared memory - for
# iox-roudi that is the segments it reserves for every client, which the clients' own VmHWM counts again when they map
# them. So adding a daemon's VmHWM to its clients' double-counts those pages: the sum is an upper bound, and RssShmem
# says by how much at most.
pid=${1:?pid}
glob=${2:?exe glob}
exe=$(readlink "/proc/$pid/exe" 2>/dev/null)
# shellcheck disable=SC2254 # the glob is deliberately unquoted: it is a pattern
case "$exe" in
$glob) ;;
*)
    echo "state=gone"
    exit 0
    ;;
esac
stat=$(cat "/proc/$pid/stat" 2>/dev/null)
status=$(cat "/proc/$pid/status" 2>/dev/null)
if [ -z "$stat" ] || [ -z "$status" ]; then
    echo "state=gone"
    exit 0
fi
tck=$(getconf CLK_TCK 2>/dev/null)
[ -n "$tck" ] || tck=100
# comm (field 2) may contain spaces and parentheses; everything after the LAST ") " is field 3 onwards.
rest=${stat##*) }
cpu=$(echo "$rest" | awk -v t="$tck" '{ printf "utime_s=%.3f stime_s=%.3f cpu_s=%.3f", $12 / t, $13 / t, ($12 + $13) / t }')
mem=$(echo "$status" | awk '
    /^VmHWM:/    { h = $2 }
    /^VmRSS:/    { r = $2 }
    /^RssShmem:/ { s = $2 }
    END { printf "vmhwm_kb=%s vmrss_kb=%s rssshmem_kb=%s", (h == "" ? "na" : h), (r == "" ? "na" : r), (s == "" ? "na" : s) }')
# The same CPU at nanosecond resolution: /proc/<pid>/task/*/schedstat's first field, summed over the threads alive now.
# utime/stime above count every thread but only in whole clock ticks (10 ms at CLK_TCK 100), which is a few percent of
# a latency cell's server; this is fine-grained but misses threads that have already exited. Both are printed and
# neither is substituted for the other: a reader can see when they disagree.
sched_ns=0
threads=0
for t in /proc/"$pid"/task/*/schedstat; do
    read -r ns _ <"$t" 2>/dev/null || continue
    sched_ns=$((sched_ns + ns))
    threads=$((threads + 1))
done
sched=$(awk -v n="$sched_ns" -v k="$threads" 'BEGIN { printf "sched_cpu_s=%.6f threads=%d", n / 1e9, k }')
echo "state=ok $cpu $sched $mem"
