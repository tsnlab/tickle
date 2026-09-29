#!/usr/bin/env bash
# build.sh <scenario> <payload> - the zenoh-pico harness, shaped like the other three frameworks' build.sh.
#
# zenoh-pico is not packaged, so this fetches and builds it into a prefix of its own the first time and
# reuses it afterwards. ZENOH_PICO_REF pins the commit, because "the version we measured" has to be a fact
# in the results rather than whatever main was that day - it is printed in the RESULT line's own build tag.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCENARIO=${1:?usage: build.sh <scenario> <payload>}
PAYLOAD=${2:?usage: build.sh <scenario> <payload>}
ZENOH_PICO_REF=${ZENOH_PICO_REF:-main}
PREFIX="$HOME/zenohpico_install"
SRC="$HOME/zenohpico_src"
SHAPE_DIR="$HERE/../tickle/common/$PAYLOAD"
CC=${CC:-gcc}

if [ ! -f "$PREFIX/lib/libzenohpico.so" ] && [ ! -f "$PREFIX/lib/libzenohpico.a" ]; then
    rm -rf "$SRC"
    git clone -q --depth 1 --branch "$ZENOH_PICO_REF" https://github.com/eclipse-zenoh/zenoh-pico.git "$SRC"
    cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
    cmake --build "$SRC/build" --parallel "$(nproc)" >/dev/null
    cmake --install "$SRC/build" >/dev/null
fi

if [ ! -f "$SHAPE_DIR/Bench.c" ]; then
    echo "No generated payload shape at $SHAPE_DIR - run 'make regen' from the repo root" >&2
    exit 1
fi
BENCH_ARRAY="$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$SHAPE_DIR/Bench.msg")"
BENCH_SAMPLE_BYTES=$((12 + BENCH_ARRAY))

SCEN_DIR="$HERE/${SCENARIO}_${PAYLOAD}"
mkdir -p "$SCEN_DIR"
# The same -DBENCH_SAMPLE_BYTES every framework's harness is built with, so the payload-boundary gate reads
# the RESULT line the same way for all four.
# ZENOH_LINUX: zenoh-pico's headers pick their system types from it, and without it transport.h fails on
# _z_sys_net_socket_t. Its own CMake sets it; a hand build has to.
CFLAGS="-O2 -Wall -Wextra -DZENOH_LINUX=1 -DBENCH_SAMPLE_BYTES=$BENCH_SAMPLE_BYTES -I$SHAPE_DIR -I$HERE/../tickle/common -I$PREFIX/include"
LIBS="-L$PREFIX/lib -lzenohpico -lpthread -lm"
for role in client server; do
    # shellcheck disable=SC2086 # CFLAGS/LIBS are deliberate word lists
    $CC $CFLAGS -o "$SCEN_DIR/$role" "$HERE/$SCENARIO/$role.c" $LIBS -Wl,-rpath,"$PREFIX/lib"
done
echo "Built $SCEN_DIR/{client,server}"
