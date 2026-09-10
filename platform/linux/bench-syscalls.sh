#!/bin/sh
# Mechanistic measurement for the optimization experiments. netns raw throughput is capped by
# veth, not TickLE, so it's too noisy to rank changes by - this instead reports, for one short
# perf pair across the netns.mk namespaces:
#   1. throughput (uninstrumented)          - sanity; HIL is the real throughput judge
#   2. perf_server syscall mix (strace -fc)  - poll/recvfrom/sendto counts + time
#   3. perf_server CPU (/usr/bin/time -v)    - user vs system seconds, context switches
# Needs `sudo make -C platform/linux createns` first.

set -u
cd "$(dirname "$0")"

BROADCAST=192.168.10.255
DUR="${DUR:-10}"
OUT="${OUT:-/tmp/bench-syscalls}"
mkdir -p "$OUT"
make -s perf >/dev/null

run() { # $1 = wrapper words for perf_server (may be empty)
    rm -f "$OUT"/perf_*.log
    # shellcheck disable=SC2086
    sudo ip netns exec ns2 $1 ./perf_server -b "$BROADCAST" -d "$DUR" </dev/null >"$OUT/perf_server.log" 2>&1 &
    spid=$!
    sleep 1
    sudo ip netns exec ns1 ./perf_client -b "$BROADCAST" -d "$((DUR + 3))" </dev/null >"$OUT/perf_client.log" 2>&1
    wait "$spid" 2>/dev/null
}

echo "=== 1. throughput (uninstrumented) ==="
run ""
grep -E '^RESULT:' "$OUT/perf_client.log" "$OUT/perf_server.log"

echo "=== 2. perf_server syscalls (strace -fc) ==="
run "strace -fc -o $OUT/strace.txt"
grep -E 'seconds|-----|recvfrom|recvmmsg|sendto|sendmsg|sendmmsg|poll|ppoll|select|total' "$OUT/strace.txt"

echo "=== 3. perf_server CPU (/usr/bin/time -v) ==="
run "/usr/bin/time -v -o $OUT/time.txt"
grep -E 'User time|System time|Percent of CPU|context switches|Maximum resident' "$OUT/time.txt"
grep -E '^RESULT: recv' "$OUT/perf_server.log"
