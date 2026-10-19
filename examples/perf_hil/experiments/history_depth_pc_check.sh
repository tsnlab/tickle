#!/usr/bin/env bash
# history_depth_burst_loss (docs/RESULTS.md rows 43-44) on the PC, in two private network namespaces joined by a veth:
# does TickLE's "beyond depth 8" cell exercise the writer's history depth at all?
#
# The CycloneDDS/FastDDS servers match, then stall consumption for -p while their KEEP_LAST 8 caches evict. TickLE's
# server sleeps -p BEFORE creating a VOLATILE Subscriber, and a VOLATILE late joiner is owed nothing written before it
# joined - so its "taken of 160" (147-148 in row 44) may be the samples published before the join, not depth eviction.
# This is the same shape as lifespan_expiry's, which lifespan_pc_check.sh showed could not fail.
#
# First run (2026-10-20, the old server): 139 of 160 at depth 8 and 139 at depth 64, 160 at -p 0 - DEPTH_BLIND. The
# server was then reshaped to the twins (server.c's header): match, stall consumption for -p into a KEEP_LAST cache of
# HISTORY_DEPTH, then take. This script now checks the reshaped server; the depth arm changes BOTH sides' depth, since
# the reader's cache is what evicts under a stalled reader (the twins' readers are KEEP_LAST 8 as well).
#
# Arms (REPS each, as row 44: -i 0.05 -n 160): depth 8 (the harness as it is) and depth 64 (copies of client.c and
# server.c with only their depth changed), at -p 3; and depth 8 at -p 0 (row 43, within depth).
#
# HOW TO READ IT, written before running the reshaped server and enforced below. KEEP_LAST predicts, at -p 3 and -i 0.05,
# STALL_N = 3 / 0.05 + 1 = 61 samples arriving while nothing is taken (the first arrival starts the stall): a cache of D
# keeps the newest D of them, so it evicts STALL_N - D (53 at depth 8, 0 at depth 64) and the reader takes 160 minus that.
#   VOID          any run without a server RESULT line, with stalled != 1, or without a client sent=160; -p 0 not taking
#                 160 in every rep; or any run whose lost != evicted (a sample missing for a reason other than eviction).
#   DEPTH_BLIND   the median taken at -p 3 differs between depth 8 and depth 64 by at most 3: the reshape did not work.
#   KEEP_LAST     depth 64's median is 160 and depth 8's median is within 3 of 160 - (STALL_N - 8) = 107: the count is
#                 the depth's, as KEEP_LAST predicts.
#   MISMATCH      the depth bites, but not by what KEEP_LAST predicts.
#
# Usage: examples/perf_hil/experiments/history_depth_pc_check.sh   (sudo -n ip; results to OUT, default under /tmp)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
H="$REPO/examples/perf_hil/tickle"
S="$H/history_depth_burst_loss"
REPS="${REPS:-3}"
WORK="${WORK:-/home/semih/tmp_build/history_depth_pc_check}"
OUT="${OUT:-/tmp/history_depth_pc_check.txt}"
NS1=hdpc_cli_$$
NS2=hdpc_srv_$$
export TMPDIR="${TMPDIR:-/home/semih/tmp_build}"

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
}
trap cleanup EXIT

: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== history_depth_pc_check $(date -Is) repo $(git -C "$REPO" rev-parse --short HEAD) ==="

rm -rf "$WORK" && mkdir -p "$WORK/d8" "$WORK/d64" "$WORK/prefix" || exit 1
(cd "$REPO" && nice make -j4 install "PREFIX=$WORK/prefix" BUILD_TYPE=release >"$WORK/make.log" 2>&1) || {
    say "BUILD FAILED (core), see $WORK/make.log"
    exit 1
}
for f in client server; do
    cp "$S/$f.c" "$WORK/d8/$f.c" && cp "$S/$f.c" "$WORK/d64/$f.c" || exit 1
done
sed -i -e 's/\.depth = 8,/.depth = 64,/' \
    -e 's/^\( *\)8, tt_RELIABLE_CACHE_ARENA_BYTES(8,/\164, tt_RELIABLE_CACHE_ARENA_BYTES(64,/' "$WORK/d64/client.c"
sed -i -e 's/^#define HISTORY_DEPTH 8$/#define HISTORY_DEPTH 64/' "$WORK/d64/server.c"
# Each variant must differ from the original in exactly those lines, or it is not the arm it claims to be.
changed=$(diff "$WORK/d8/client.c" "$WORK/d64/client.c" | grep -c '^>')
[ "$changed" = 2 ] || {
    say "VARIANT WRONG: depth-64 client differs in $changed lines, not 2"
    exit 1
}
changed=$(diff "$WORK/d8/server.c" "$WORK/d64/server.c" | grep -c '^>')
[ "$changed" = 1 ] || {
    say "VARIANT WRONG: depth-64 server differs in $changed lines, not 1"
    exit 1
}
export PKG_CONFIG_PATH="$WORK/prefix/lib/pkgconfig"
ARRAY="$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$H/common/p1/Bench.msg")"
# shellcheck disable=SC2207 # pkg-config's words are the flags
CFLAGS=(-O2 -DBENCH_CORE_BUILD=release -DBENCH_SAMPLE_PATH=datagram -DBENCH_DATAGRAM_BYTES=1472
    "-DBENCH_SAMPLE_BYTES=$((12 + ARRAY))" "-I$H/common/p1" "-I$H/common" $(pkg-config --cflags tickle))
# shellcheck disable=SC2207
LIBS=($(pkg-config --libs tickle) -lm -lpthread)
for target in d8/server d8/client d64/server d64/client; do
    src="$WORK/$target.c"
    if ! gcc "${CFLAGS[@]}" -o "$WORK/$target" "$src" "$H/common/p1/Bench.c" "${LIBS[@]}" >>"$WORK/build.log" 2>&1; then
        say "BUILD FAILED ($target), see $WORK/build.log"
        exit 1
    fi
done
say "built depth-8 and depth-64 servers and clients"

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add hdpc1 netns "$NS1" type veth peer name hdpc2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev hdpc1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev hdpc2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set hdpc1 up
sudo -n ip -n "$NS2" link set hdpc2 up

run_one() { # $1 depth arm (d8|d64) $2 pause $3 tag; prints "taken sent evicted lost" or VOID <why>
    local slog="$WORK/$3.server.log" clog="$WORK/$3.client.log"
    # Launched together, as run_scenario.sh with PRE_CLIENT_SLEEP=0 does for this scenario.
    sudo -n ip netns exec "$NS2" timeout -s INT 18 "$WORK/$1/server" -i 0.05 -n 160 -p "$2" 2>&1 | head -c 100000 >"$slog" &
    local spipe=$!
    sudo -n ip netns exec "$NS1" timeout 25 "$WORK/$1/client" -i 0.05 -n 160 -p "$2" 2>&1 | head -c 100000 >"$clog"
    wait "$spipe"
    local line taken sent evicted lost stalled
    line="$(grep -a '^RESULT: .*role=server' "$slog" | tail -1)"
    taken="$(echo "$line" | grep -o ' recv=[0-9]*' | grep -o '[0-9]*$')"
    evicted="$(echo "$line" | grep -o ' evicted=[0-9]*' | grep -o '[0-9]*$')"
    lost="$(echo "$line" | grep -o ' lost=[0-9]*' | grep -o '[0-9]*$')"
    stalled="$(echo "$line" | grep -o ' stalled=[0-9]*' | grep -o '[0-9]*$')"
    sent="$(grep -a -o 'role=client sent=[0-9]*' "$clog" | grep -o '[0-9]*$')"
    if [ -z "$taken" ] || [ -z "$evicted" ] || [ -z "$lost" ] || [ "${sent:-}" != 160 ]; then
        echo "VOID no server RESULT with recv/evicted/lost, or client sent=${sent:-none}"
    elif [ "$stalled" != 1 ]; then
        echo "VOID stalled=${stalled:-none}"
    elif [ "$lost" != "$evicted" ]; then
        echo "VOID lost $lost != evicted $evicted"
    else
        echo "$taken $sent $evicted $lost"
    fi
}

declare -A TAKEN
VOID=0
for rep in $(seq 1 "$REPS"); do
    for spec in "d8 3" "d64 3" "d8 0"; do
        read -r arm pause <<<"$spec"
        r="$(run_one "$arm" "$pause" "${arm}_p${pause}_r$rep")"
        say "${arm}_p${pause}_r$rep: taken sent evicted lost = $r"
        if [ "${r%% *}" = VOID ]; then
            VOID=1
            continue
        fi
        TAKEN["$arm $pause"]+="${r%% *} "
    done
done
median() { tr ' ' '\n' | grep -v '^$' | sort -n | awk '{ a[NR] = $1 } END { print (NR ? a[int((NR + 1) / 2)] : "") }'; }
m8="$(echo "${TAKEN[d8 3]:-}" | median)"
m64="$(echo "${TAKEN[d64 3]:-}" | median)"
say "taken at -p 3: depth 8 [${TAKEN[d8 3]:-}] median $m8; depth 64 [${TAKEN[d64 3]:-}] median $m64; -p 0 [${TAKEN[d8 0]:-}]"
for t in ${TAKEN[d8 0]:-}; do [ "$t" = 160 ] || VOID=1; done
[ "$m8" = 160 ] && VOID=1
if [ "$VOID" = 1 ] || [ -z "$m8" ] || [ -z "$m64" ]; then
    say "VERDICT: VOID"
    exit 2
fi
STALL_N=61
EXP8=$((160 - (STALL_N - 8)))
if [ $((m8 - m64)) -le 3 ] && [ $((m64 - m8)) -le 3 ]; then
    say "VERDICT: DEPTH_BLIND - the taken count does not depend on the history depth; the reshape did not work"
elif [ "$m64" = 160 ] && [ $((m8 - EXP8)) -le 3 ] && [ $((EXP8 - m8)) -le 3 ]; then
    say "VERDICT: KEEP_LAST - depth 64 took 160, depth 8 took $m8 (predicted $EXP8 +- 3): the count is the depth's"
else
    say "VERDICT: MISMATCH - the depth bites, but depth 8 took $m8 (predicted $EXP8 +- 3) and depth 64 $m64 (predicted 160)"
fi
