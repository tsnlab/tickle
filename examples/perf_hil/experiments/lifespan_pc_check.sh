#!/usr/bin/env bash
# lifespan_expiry (docs/RESULTS.md row 47) on the PC, in two private network namespaces joined by a veth: does the
# TickLE harness measure LIFESPAN at all, before and after its 2026-10-20 reshape?
#
# Before, the server slept -p BEFORE creating a VOLATILE Subscriber, so what it counted as lost was what was published
# before it joined - the lifespan could not change it. After, it matches, stalls consumption for -p and then takes,
# dropping what has outlived its lifespan, as the CycloneDDS/FastDDS twins do.
#
# Arms (REPS each, -i 0.02 -n 250 as run_perf.sh; T = the writer's lifespan, p = the stall):
#   old T=0.1 p=1.0, old T=1000 p=1.0  the pre-reshape harness (origin/main's two sources, or OLD_SRC)
#   new T=0.1 p=1.0, new T=0.1 p=2.0   the reshaped harness
#   new T=1000 p=1.0                    control: no lifespan to bite, so nothing may be lost
#   new T=0.1 p=0.05                    within the lifespan: nothing may be lost
#
# HOW TO READ IT, written before running and enforced in verdict() below:
#   OLD_CANNOT_FAIL  the old arm's median lost at T=0.1 and at T=1000 differ by at most 3: the old scenario's count
#                    does not depend on the lifespan, i.e. it never measured LIFESPAN. A larger difference refutes that.
#   NEW_MEASURES     every new run has stalled=1 and queue_overflow=0 (else VOID); T=1000 p=1.0 and T=0.1 p=0.05
#                    lose 0 in every rep; T=0.1 loses (p - 0.1)/0.02 +/- 5 in every rep (45 at p=1.0, 95 at p=2.0,
#                    CycloneDDS's 45/95 on the rig), and lost == expired there (no loss but LIFESPAN's).
#   Any arm whose run printed no server RESULT line, or whose client did not report sent=250, is VOID - not a pass.
#
# Usage: examples/perf_hil/experiments/lifespan_pc_check.sh   (sudo -n ip; results to OUT, default under /tmp)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
H="$REPO/examples/perf_hil/tickle"
REPS="${REPS:-3}"
WORK="${WORK:-/home/semih/tmp_build/lifespan_pc_check}"
OUT="${OUT:-/tmp/lifespan_pc_check.txt}"
OLD_REF="${OLD_REF:-b39d8568}"
NS1=lspc_cli_$$
NS2=lspc_srv_$$
export TMPDIR="${TMPDIR:-/home/semih/tmp_build}"

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
}
trap cleanup EXIT

: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== lifespan_pc_check $(date -Is) repo $(git -C "$REPO" rev-parse --short HEAD) old $OLD_REF ==="

# Build: core once into a private prefix (never the shared ~/tickle_local_install*), both arms against it.
rm -rf "$WORK" && mkdir -p "$WORK/old" "$WORK/new" "$WORK/prefix" || exit 1
(cd "$REPO" && nice make -j4 install "PREFIX=$WORK/prefix" BUILD_TYPE=release >"$WORK/make.log" 2>&1) || {
    say "BUILD FAILED (core), see $WORK/make.log"
    exit 1
}
git -C "$REPO" show "$OLD_REF:examples/perf_hil/tickle/lifespan_expiry/server.c" >"$WORK/old/server.c" || exit 1
git -C "$REPO" show "$OLD_REF:examples/perf_hil/tickle/lifespan_expiry/client.c" >"$WORK/old/client.c" || exit 1
cp "$H/lifespan_expiry/server.c" "$H/lifespan_expiry/client.c" "$WORK/new/" || exit 1
export PKG_CONFIG_PATH="$WORK/prefix/lib/pkgconfig"
ARRAY="$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$H/common/p1/Bench.msg")"
# shellcheck disable=SC2207 # pkg-config's words are the flags
CFLAGS=(-O2 -DBENCH_CORE_BUILD=release -DBENCH_SAMPLE_PATH=datagram -DBENCH_DATAGRAM_BYTES=1472
    "-DBENCH_SAMPLE_BYTES=$((12 + ARRAY))" "-I$H/common/p1" "-I$H/common" $(pkg-config --cflags tickle))
# shellcheck disable=SC2207
LIBS=($(pkg-config --libs tickle) -lm -lpthread)
for arm in old new; do
    for role in server client; do
        gcc "${CFLAGS[@]}" -o "$WORK/$arm/$role" "$WORK/$arm/$role.c" "$H/common/p1/Bench.c" "${LIBS[@]}" \
            >>"$WORK/build.log" 2>&1 || {
            say "BUILD FAILED ($arm $role), see $WORK/build.log"
            exit 1
        }
    done
done
say "built old and new"

# Two namespaces, the addresses the harness hard-codes its broadcast for (192.168.10.255), as on the rig.
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add lspc1 netns "$NS1" type veth peer name lspc2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev lspc1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev lspc2
for ns in "$NS1" "$NS2"; do
    sudo -n ip -n "$ns" link set lo up
done
sudo -n ip -n "$NS1" link set lspc1 up
sudo -n ip -n "$NS2" link set lspc2 up

# One run: prints "lost expired stalled overflow recv sent", or VOID.
run_one() { # $1 arm $2 lifespan $3 pause $4 tag
    local slog="$WORK/$4.server.log" clog="$WORK/$4.client.log"
    # timeout -s INT ends the server as its own SIGINT handler expects, so it still prints RESULT; output capped.
    sudo -n ip netns exec "$NS2" timeout -s INT 12 "$WORK/$1/server" -p "$3" -T "$2" -d 10 2>&1 | head -c 100000 >"$slog" &
    local spipe=$!
    sudo -n ip netns exec "$NS1" timeout 15 "$WORK/$1/client" -i 0.02 -T "$2" -n 250 2>&1 | head -c 100000 >"$clog"
    wait "$spipe"
    local res sent
    res="$(grep -a '^RESULT: .*role=server' "$slog" | tail -1)"
    sent="$(grep -a -o 'role=client sent=[0-9]*' "$clog" | grep -o '[0-9]*$')"
    if [ -z "$res" ] || [ "${sent:-}" != "250" ]; then
        echo "VOID"
        return
    fi
    f() { echo "$res" | grep -o " $1=[0-9]*" | grep -o '[0-9]*$'; }
    echo "$(f lost) $(f expired) $(f stalled) $(f queue_overflow) $(f recv) $sent"
}

declare -A LOST
VOIDS=0
FAILS=0
for rep in $(seq 1 "$REPS"); do
    for spec in "old 0.1 1.0" "old 1000 1.0" "new 0.1 1.0" "new 0.1 2.0" "new 1000 1.0" "new 0.1 0.05"; do
        read -r arm life pause <<<"$spec"
        tag="${arm}_T${life}_p${pause}_r${rep}"
        r="$(run_one "$arm" "$life" "$pause" "$tag")"
        say "$tag: lost expired stalled overflow recv sent = $r"
        if [ "$r" = "VOID" ]; then
            VOIDS=$((VOIDS + 1))
            continue
        fi
        read -r lost expired stalled overflow _recv _sent <<<"$r"
        LOST["$arm $life $pause"]+="$lost "
        if [ "$arm" = new ]; then
            if [ "${stalled:-0}" != 1 ] || [ "${overflow:-1}" != 0 ]; then
                say "  VOID: stalled=$stalled overflow=$overflow"
                VOIDS=$((VOIDS + 1))
                continue
            fi
            case "$life $pause" in
            "1000 1.0" | "0.1 0.05")
                [ "$lost" = 0 ] || {
                    say "  FAIL: expected 0 lost"
                    FAILS=$((FAILS + 1))
                }
                ;;
            "0.1 1.0" | "0.1 2.0")
                want=$(awk -v p="$pause" 'BEGIN { printf "%d", (p - 0.1) / 0.02 + 0.5 }')
                if [ $((lost - want)) -gt 5 ] || [ $((want - lost)) -gt 5 ] || [ "$lost" != "$expired" ]; then
                    say "  FAIL: expected lost $want +/- 5 and lost == expired"
                    FAILS=$((FAILS + 1))
                fi
                ;;
            esac
        fi
    done
done

median() { tr ' ' '\n' | grep -v '^$' | sort -n | awk '{ a[NR] = $1 } END { print (NR ? a[int((NR + 1) / 2)] : "") }'; }
m01="$(echo "${LOST[old 0.1 1.0]:-}" | median)"
m1000="$(echo "${LOST[old 1000 1.0]:-}" | median)"
say "old median lost: T=0.1 $m01, T=1000 $m1000"
if [ -z "$m01" ] || [ -z "$m1000" ]; then
    say "OLD: VOID (no runs)"
elif [ $((m01 - m1000)) -le 3 ] && [ $((m1000 - m01)) -le 3 ]; then
    say "OLD_CANNOT_FAIL: the old count does not depend on the lifespan"
else
    say "OLD_MEASURED_SOMETHING: refutes the reading that the old scenario never measured LIFESPAN"
fi
if [ "$VOIDS" -gt 0 ]; then
    say "VERDICT: VOID ($VOIDS void runs)"
    exit 2
elif [ "$FAILS" -gt 0 ]; then
    say "VERDICT: NEW_FAILS ($FAILS)"
    exit 1
fi
say "VERDICT: NEW_MEASURES (every new arm as pre-registered)"
