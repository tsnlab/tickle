#!/bin/sh
# Mechanistic measurement for the optimization experiments: run one short perf pair across the
# netns.mk namespaces with perf_server under `strace -fc`, so the syscall mix (poll/recvfrom/
# sendto counts + time) is visible even when raw throughput is capped by veth rather than by
# TickLE. Prints the strace summary plus the throughput line. Needs `sudo make createns` first.

set -u
cd "$(dirname "$0")"

BROADCAST=192.168.10.255
DUR="${DUR:-8}"
OUT="${OUT:-/tmp/bench-syscalls}"
mkdir -p "$OUT"

make -s perf >/dev/null
rm -f "$OUT"/*.log

sudo ip netns exec ns2 strace -fc -o "$OUT/strace_server.txt" \
    ./perf_server -b "$BROADCAST" -d "$DUR" </dev/null >"$OUT/perf_server.log" 2>&1 &
spid=$!
sleep 1
sudo ip netns exec ns1 ./perf_client -b "$BROADCAST" -d "$((DUR + 3))" </dev/null >"$OUT/perf_client.log" 2>&1
wait "$spid" 2>/dev/null

echo "=== throughput ==="
grep -E '^RESULT:' "$OUT/perf_client.log" "$OUT/perf_server.log"
echo "=== perf_server syscalls (strace -fc) ==="
# Header + the rows for the calls that matter on the hot path, plus the totals line.
grep -E 'seconds|-----|recvfrom|recvmmsg|sendto|sendmsg|sendmmsg|poll|ppoll|select|^100\.00|total' "$OUT/strace_server.txt"
