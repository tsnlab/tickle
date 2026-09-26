#!/usr/bin/env bash
# Verifies 1af57e3e (tt_try_receive no longer probes a known-empty socket) against the
# pre-registration written in examples/perf_hil/CORE_HEADROOM.md before the change existed.
#
# HOW TO READ IT, and this is the pre-registration verbatim rather than a restatement:
#   "the server's receive syscalls per sample should fall toward CycloneDDS's 0.74 and server
#    stime_s should fall roughly in proportion. If stime_s does NOT fall when the syscall count
#    does, then per-syscall entry cost was not what the kernel time was, and sendmmsg should be
#    dropped rather than attempted on the same reasoning."
#   Dev's own added bound: n+1 recvfrom per drain cannot go below ~1.0 per sample without
#   recvmmsg, so ~1.0 is success here and 0.74 is the NEXT step's target, not this one's.
#
#   FAIL-TO-CONFIRM outcome, named in advance: syscalls/sample down, stime_s flat. That kills
#   sendmmsg as well, and is the cheaper place to learn it. It is a real possible result, not a
#   formality - the kernel time could be per-BYTE (copy, checksum) rather than per-ENTRY.
#
# Two runs per arm, because strace inflates CPU and must not be the source of the stime figure:
#   (a) clean run  -> stime_s, utime_s, throughput. No strace anywhere.
#   (b) strace run -> exact syscall counts. Times from it are ignored.
# Also logs datagrams-per-drain so Dev can size recvmmsg's buffer count N from a distribution
# rather than a guess.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT=${OUT:-/tmp/recv_probe_verify_$(date +%Y-%m-%d).txt}
BEFORE=${BEFORE:-23465a68}; AFTER=${AFTER:-1af57e3e}
REPS=${REPS:-3}
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
kill_srv() { sh_ "$SERVER" "pkill -INT -x server" >/dev/null 2>&1 || true; sleep 2; }
trap kill_srv EXIT
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== receive-probe verification, $(date -Is) ==="
say "before=$BEFORE   after=$AFTER (${LABEL:-blind two-socket probe removed})"

build() {
    local sha="$1" pids=() bad=0
    say ""; say "--- building $sha ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle
TICKLE_CORE_BUILD=release ./build.sh reliable_throughput p1 >/tmp/rpv_build.log 2>&1 \
  || { echo BUILD-FAIL on \$(hostname); tail -6 /tmp/rpv_build.log; exit 1; }" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD FAILED - stopping"; exit 1; }
    # Full SHAs on both sides (2026-09-26): comparing the rig's --short HEAD against a full SHA passed in
    # by the caller refused a correct build as an identity failure.
    local head; head=$(sh_ "$CLIENT" "git -C ~/tickle rev-parse HEAD")
    sha=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse "$sha")
    say "  identity: HEAD=$head (wanted $sha)"
    [ "$head" = "$sha" ] || { say "  IDENTITY FAIL - stopping"; exit 1; }
}
run() { # $1 tag, $2 strace(0|1)
    local tag="$1" st="$2" dir="tickle/examples/perf_hil/tickle/reliable_throughput_p1" pre=""
    kill_srv
    [ "$st" = 1 ] && pre="strace -c -f -o /tmp/rpv_srv_sc.txt"
    sh_ "$SERVER" "rm -f /tmp/rpv_srv_sc.txt; cd ~/$dir; nohup $pre taskset -c 1-3 ./server -d 12 -Q > /tmp/rpv_srv.log 2>&1 < /dev/null &"
    sleep 3
    sh_ "$CLIENT" "cd ~/$dir && taskset -c 1-3 ./client -d 5 -Q" >/dev/null 2>&1
    sleep 4; kill_srv
    local srv; srv=$(sh_ "$SERVER" "grep -m1 '^RESULT:' /tmp/rpv_srv.log" 2>/dev/null)
    case "$srv" in *core_build=release*) ;; *) say "  $tag IDENTITY FAIL: not release -> ${srv:-no RESULT}"; return;; esac
    local recv; recv=$(grep -oE 'recv=[0-9]+' <<<"$srv" | head -1 | cut -d= -f2)
    if [ "$st" = 1 ]; then
        local rc; rc=$(sh_ "$SERVER" "awk '/recvfrom|recvmsg|recvmmsg/{s+=\$4} END{print s+0}' /tmp/rpv_srv_sc.txt")
        local er; er=$(sh_ "$SERVER" "awk '/recvfrom|recvmsg|recvmmsg/{s+=\$5} END{print s+0}' /tmp/rpv_srv_sc.txt")
        say "  $tag STRACE  recv=$recv  recv_syscalls=$rc  eagain=$er  per_sample=$(awk -v a="$rc" -v b="$recv" 'BEGIN{if(b>0)printf "%.3f",a/b; else print "?"}')"
    else
        say "  $tag CLEAN   $(grep -oE '(recv|utime_s|stime_s|cpu_s_per_Msample|peak_rss_kb)=[0-9.]+' <<<"$srv" | tr '\n' ' ')"
    fi
}
for sha in "$BEFORE" "$AFTER"; do
    build "$sha"
    tag=$([ "$sha" = "$BEFORE" ] && echo BEFORE || echo AFTER)
    for r in $(seq 1 "$REPS"); do run "$tag rep$r" 0; done
    run "$tag" 1
done
say ""; say "=== done ==="
