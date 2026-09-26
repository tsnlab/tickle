#!/usr/bin/env bash
# How much of TickLE's CPU is one-datagram-per-syscall, and is it batchable?
#
# WHY: at release on the rig, the TickLE throughput client spends utime 0.923s against stime 2.253s
# - 71% of its CPU is kernel time - while CycloneDDS sits at 55% and FastDDS at 48%. TickLE's own
# user-space code is already the smallest of the three (0.97us/sample against CycloneDDS's 4.9),
# so the remaining core headroom is not in our code: it is in how many times we enter the kernel.
# src/hal_linux.c sends with one sendto() per datagram and receives with one recvfrom(); there is
# no sendmmsg/recvmmsg anywhere in the tree.
#
# HOW TO READ IT, written before running:
#   - sendto_calls / samples ~= 1.0  -> one syscall per sample, batching is available, and the
#     ceiling on the win is (1 - 1/batch) of the send-side kernel time.
#   - sendto_calls / samples << 1.0  -> something already batches, and the kernel time is NOT
#     syscall entry count; the whole sendmmsg idea is then unsupported and must be dropped.
#   - CONTROL, and the reason this is not just a TickLE measurement: the same count for CycloneDDS.
#     If CycloneDDS also issues ~1 send syscall per sample yet runs at 55% system time, then
#     syscall count is not what separates them and batching would not close the gap either. If
#     CycloneDDS issues far fewer, that is direct evidence the batching is what buys its lower
#     kernel share, i.e. a known-good technique rather than a guess.
#   - Receive side is read the same way from recvfrom/recvmsg counts on the SERVER.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT=${OUT:-/tmp/syscall_headroom_$(date +%Y-%m-%d).txt}
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== syscall batching headroom, $(date -Is) ==="
say "head=$(git -C "$REPO" rev-parse --short origin/main)"

# Build both first: `git clean -fdqx` from earlier experiments leaves only sources behind, and the
# first version of this script straced a framework whose binaries did not exist. strace then left
# the PREVIOUS framework's counts in /tmp/sc_cli.txt and the script printed them again under the new
# name - two byte-identical blocks that would read as "the two frameworks behave the same". The
# md5 check below is what makes that impossible to publish rather than merely unlikely.
for fw in tickle cyclonedds; do
    say ""; say "--- building $fw ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "cd ~/tickle/examples/perf_hil/$fw && TICKLE_CORE_BUILD=release ./build.sh reliable_throughput p1 >/tmp/sc_build_$fw.log 2>&1 || { echo BUILD-FAIL; tail -5 /tmp/sc_build_$fw.log; }" | sed 's/^/    /' | tee -a "$OUT"
    done
done

prev_cli=""; prev_srv=""
for fw in tickle cyclonedds; do
    dir="tickle/examples/perf_hil/$fw/reliable_throughput_p1"
    say ""; say "--- $fw ---"
    sh_ "$CLIENT" "rm -f /tmp/sc_cli.txt"; sh_ "$SERVER" "rm -f /tmp/sc_srv.txt"
    if ! sh_ "$CLIENT" "test -x ~/$dir/client"; then say "  NO CLIENT BINARY at $dir - skipping, not reporting"; continue; fi
    sh_ "$SERVER" "pkill -INT -x server" >/dev/null 2>&1 || true; sleep 2
    sh_ "$SERVER" "cd ~/$dir; nohup strace -c -f -o /tmp/sc_srv.txt taskset -c 1-3 ./server -d 12 -Q > /tmp/sc_srv.log 2>&1 < /dev/null &"
    sleep 3
    res=$(sh_ "$CLIENT" "cd ~/$dir && strace -c -f -o /tmp/sc_cli.txt taskset -c 1-3 ./client -d 5 -Q" 2>&1 | grep -m1 '^RESULT:')
    sleep 4; sh_ "$SERVER" "pkill -INT -x server" >/dev/null 2>&1 || true; sleep 2
    sent=$(grep -oE 'sent=[0-9]+' <<<"$res" | head -1 | cut -d= -f2)
    recv=$(sh_ "$SERVER" "grep -m1 '^RESULT:' /tmp/sc_srv.log" 2>/dev/null | grep -oE 'recv=[0-9]+' | head -1 | cut -d= -f2)
    say "  client sent=$sent   server recv=${recv:-?}"
    cli_md5=$(sh_ "$CLIENT" "md5sum /tmp/sc_cli.txt 2>/dev/null | cut -d' ' -f1")
    srv_md5=$(sh_ "$SERVER" "md5sum /tmp/sc_srv.txt 2>/dev/null | cut -d' ' -f1")
    if [ -z "$cli_md5" ] || [ "$cli_md5" = "$prev_cli" ] || [ "$srv_md5" = "$prev_srv" ]; then
        say "  IDENTITY FAIL: strace output missing or identical to the previous framework's - not reporting"
        continue
    fi
    prev_cli="$cli_md5"; prev_srv="$srv_md5"
    say "  client syscalls:"
    sh_ "$CLIENT" "awk 'NR<=4 || /send|recv|poll|ioctl|futex/' /tmp/sc_cli.txt" | sed 's/^/    /' | tee -a "$OUT"
    say "  server syscalls:"
    sh_ "$SERVER" "awk 'NR<=4 || /send|recv|poll|ioctl|futex/' /tmp/sc_srv.txt" | sed 's/^/    /' | tee -a "$OUT"
done
say ""; say "=== done ==="
