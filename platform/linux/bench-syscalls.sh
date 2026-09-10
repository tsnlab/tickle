#!/bin/sh
# Mechanistic measurement for the optimization experiments. netns raw throughput is capped by
# veth, not TickLE, so it's too noisy to rank changes by - this instead reports, for one short
# perf pair across the netns.mk namespaces:
#   1. throughput (no instrumentation)                     - sanity / HIL is the real judge
#   2. perf_server syscall mix (strace -fc)                 - poll/recvfrom/sendto counts + time
#   3. perf_server instructions + cycles (perf stat)        - CPU work per run, for memcpy/decode changes
# Needs `sudo make -C platform/linux createns` first.

set -u
cd "$(dirname "$0")"

BROADCAST=192.168.10.255
DUR="${DUR:-10}"
OUT="${OUT:-/tmp/bench-syscalls}"
mkdir -p "$OUT"
make -s perf >/dev/null

run() { # $1 = wrapper for perf_server (may be empty)
    rm -f "$OUT"/*.log
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

echo "=== 2. perf_server syscalls ==="
run "strace -fc -o $OUT/strace.txt"
grep -E 'seconds|-----|recvfrom|recvmmsg|sendto|sendmsg|sendmmsg|poll|ppoll|select|total' "$OUT/strace.txt"

echo "=== 3. perf_server CPU (perf stat) ==="
run "perf stat -o $OUT/perfstat.txt --"
grep -E 'instructions|cycles|task-clock|seconds time elapsed|insn per cycle' "$OUT/perfstat.txt"
echo "recv this run:"; grep -E '^RESULT: recv' "$OUT/perf_server.log"
