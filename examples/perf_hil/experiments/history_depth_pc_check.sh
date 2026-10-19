#!/usr/bin/env bash
# history_depth_burst_loss (docs/RESULTS.md rows 43-44) on the PC, in two private network namespaces joined by a veth:
# does TickLE's "beyond depth 8" cell exercise the writer's history depth at all?
#
# The CycloneDDS/FastDDS servers match, then stall consumption for -p while their KEEP_LAST 8 caches evict. TickLE's
# server sleeps -p BEFORE creating a VOLATILE Subscriber, and a VOLATILE late joiner is owed nothing written before it
# joined - so its "taken of 160" (147-148 in row 44) may be the samples published before the join, not depth eviction.
# This is the same shape as lifespan_expiry's, which lifespan_pc_check.sh showed could not fail.
#
# Arms (REPS each, as row 44: -i 0.05 -n 160): the writer at depth 8 (the harness as it is) and at depth 64 (the same
# client.c with only its depth changed, on a copy), at -p 3; and depth 8 at -p 0 (row 43, within depth).
#
# HOW TO READ IT, written before running and enforced below:
#   VOID          any run without a server RESULT line or a client sent=160; -p 0 not taking 160 in every rep; or -p 3
#                 at depth 8 taking 160 (no stall effect at all - nothing to read).
#   DEPTH_BLIND   the median taken at -p 3 differs between depth 8 and depth 64 by at most 3: row 44's TickLE count does
#                 not depend on the history depth, so it is not a "beyond depth" figure.
#   DEPTH_BITES   a larger difference: the depth governs it, and the reading above is refuted.
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
cp "$S/client.c" "$WORK/d8/client.c" && cp "$S/client.c" "$WORK/d64/client.c" || exit 1
sed -i -e 's/\.depth = 8,/.depth = 64,/' \
    -e 's/^\( *\)8, tt_RELIABLE_CACHE_ARENA_BYTES(8,/\164, tt_RELIABLE_CACHE_ARENA_BYTES(64,/' "$WORK/d64/client.c"
# The variant must differ from the original in exactly those two lines, or it is not the arm it claims to be.
changed=$(diff "$WORK/d8/client.c" "$WORK/d64/client.c" | grep -c '^>')
[ "$changed" = 2 ] || {
    say "VARIANT WRONG: depth-64 client differs in $changed lines, not 2"
    exit 1
}
export PKG_CONFIG_PATH="$WORK/prefix/lib/pkgconfig"
ARRAY="$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$H/common/p1/Bench.msg")"
# shellcheck disable=SC2207 # pkg-config's words are the flags
CFLAGS=(-O2 -DBENCH_CORE_BUILD=release -DBENCH_SAMPLE_PATH=datagram -DBENCH_DATAGRAM_BYTES=1472
    "-DBENCH_SAMPLE_BYTES=$((12 + ARRAY))" "-I$H/common/p1" "-I$H/common" $(pkg-config --cflags tickle))
# shellcheck disable=SC2207
LIBS=($(pkg-config --libs tickle) -lm -lpthread)
for target in server d8/client d64/client; do
    src="$S/server.c"
    [ "$target" = server ] || src="$WORK/$target.c"
    if ! gcc "${CFLAGS[@]}" -o "$WORK/$target" "$src" "$H/common/p1/Bench.c" "${LIBS[@]}" >>"$WORK/build.log" 2>&1; then
        say "BUILD FAILED ($target), see $WORK/build.log"
        exit 1
    fi
done
say "built server, depth-8 and depth-64 clients"

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add hdpc1 netns "$NS1" type veth peer name hdpc2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev hdpc1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev hdpc2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set hdpc1 up
sudo -n ip -n "$NS2" link set hdpc2 up

run_one() { # $1 depth arm (d8|d64) $2 pause $3 tag; prints "taken sent" or VOID
    local slog="$WORK/$3.server.log" clog="$WORK/$3.client.log"
    # Launched together, as run_scenario.sh with PRE_CLIENT_SLEEP=0 does for this scenario.
    sudo -n ip netns exec "$NS2" timeout -s INT 18 "$WORK/server" -i 0.05 -n 160 -p "$2" 2>&1 | head -c 100000 >"$slog" &
    local spipe=$!
    sudo -n ip netns exec "$NS1" timeout 25 "$WORK/$1/client" -i 0.05 -n 160 -p "$2" 2>&1 | head -c 100000 >"$clog"
    wait "$spipe"
    local taken sent
    taken="$(grep -a '^RESULT: .*role=server' "$slog" | tail -1 | grep -o ' recv=[0-9]*' | grep -o '[0-9]*$')"
    sent="$(grep -a -o 'role=client sent=[0-9]*' "$clog" | grep -o '[0-9]*$')"
    if [ -z "$taken" ] || [ "${sent:-}" != 160 ]; then
        echo VOID
    else
        echo "$taken $sent"
    fi
}

declare -A TAKEN
VOID=0
for rep in $(seq 1 "$REPS"); do
    for spec in "d8 3" "d64 3" "d8 0"; do
        read -r arm pause <<<"$spec"
        r="$(run_one "$arm" "$pause" "${arm}_p${pause}_r$rep")"
        say "${arm}_p${pause}_r$rep: taken sent = $r"
        if [ "$r" = VOID ]; then
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
if [ $((m8 - m64)) -le 3 ] && [ $((m64 - m8)) -le 3 ]; then
    say "VERDICT: DEPTH_BLIND - row 44's TickLE count does not depend on the history depth"
else
    say "VERDICT: DEPTH_BITES - the depth governs the count; the late-join reading is refuted"
fi
