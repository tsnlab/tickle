#!/usr/bin/env bash
# build.sh <scenario> <payload> - the zenoh-pico harness, shaped like the other three frameworks' build.sh.
#
# zenoh-pico is not packaged, so this fetches and builds it into a prefix of its own the first time and
# reuses it afterwards.
#
# ZENOH_PICO_REF names the ref to clone. It defaults to main, so it does NOT pin a commit - the version a rig
# actually measured is whatever main was on the day that rig first built, and the only way to know it is to ask
# the rig (`git log -1` in $SRC). The earlier wording here claimed the ref was pinned and printed "in the RESULT
# line's own build tag"; neither is true. The RESULT line has no build tag, and its `transport=tcp` is a literal
# in the printf, so nothing in a result says which library produced it. Both rig hosts were checked by hand on
# 2026-09-30 and agree at d9b4eea (release 1.10.1), which is what every zenoh figure in COMPARISON.md refers to.
#
# ZENOH_DEBUG turns zenoh-pico's own logging on (1 error, 2 info, 3 debug). It matters more than it looks:
# WITHOUT it every _Z_LOG call in the library compiles to nothing, so a link that closes mid-run says nothing
# anywhere, and the logging that does exist goes to stdout via printf, not to stderr. A harness that keeps only
# `^RESULT` lines therefore cannot see a failure even in a build that would have reported one.
#
# A logging build is a DIAGNOSTIC build and its numbers must never be published: the logging sits on the data
# path. So it gets a prefix of its own (ZENOH_PICO_PREFIX), and the prefix records the level it was built at and
# refuses to be reused at a different one - because with no build tag in the RESULT line, a logging library left
# in the measurement prefix would produce numbers indistinguishable from clean ones.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCENARIO=${1:?usage: build.sh <scenario> <payload>}
PAYLOAD=${2:?usage: build.sh <scenario> <payload>}
ZENOH_PICO_REF=${ZENOH_PICO_REF:-main}
ZENOH_DEBUG=${ZENOH_DEBUG:-}
PREFIX=${ZENOH_PICO_PREFIX:-$HOME/zenohpico_install}
SRC=${ZENOH_PICO_SRC:-$HOME/zenohpico_src}
SHAPE_DIR="$HERE/../tickle/common/$PAYLOAD"
CC=${CC:-gcc}

MARKER="$PREFIX/.tickle_zenoh_debug"
WANT_DEBUG=${ZENOH_DEBUG:-none}
if [ -f "$PREFIX/lib/libzenohpico.so" ] || [ -f "$PREFIX/lib/libzenohpico.a" ]; then
    # A prefix from before this marker existed can only have been built with logging off, because no earlier
    # version of this script could pass ZENOH_DEBUG at all. That is why "no marker" reads as "none" instead of
    # refusing: it grandfathers the rig's existing measurement prefixes without weakening the check.
    HAVE_DEBUG=$(cat "$MARKER" 2>/dev/null || echo none)
    if [ "$HAVE_DEBUG" != "$WANT_DEBUG" ]; then
        echo "REFUSING: $PREFIX holds a zenoh-pico built with logging '$HAVE_DEBUG', this run asks for '$WANT_DEBUG'." >&2
        echo "Nothing in a RESULT line says which library produced it, so reusing one for the other would make a" >&2
        echo "diagnostic run and a measurement run indistinguishable. Give the diagnostic build its own prefix:" >&2
        echo "  ZENOH_DEBUG=1 ZENOH_PICO_PREFIX=\$HOME/zenohpico_install_dbg ZENOH_PICO_SRC=\$HOME/zenohpico_src_dbg ./build.sh ..." >&2
        exit 1
    fi
else
    rm -rf "$SRC"
    git clone -q --depth 1 --branch "$ZENOH_PICO_REF" https://github.com/eclipse-zenoh/zenoh-pico.git "$SRC"
    debug_arg=()
    [ -n "$ZENOH_DEBUG" ] && debug_arg=(-DZENOH_DEBUG="$ZENOH_DEBUG")
    cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        ${debug_arg[0]+"${debug_arg[@]}"} >/dev/null
    cmake --build "$SRC/build" --parallel "$(nproc)" >/dev/null
    cmake --install "$SRC/build" >/dev/null
    printf '%s\n' "$WANT_DEBUG" >"$MARKER"
    echo "Built zenoh-pico $(git -C "$SRC" log --oneline -1) into $PREFIX with logging=$WANT_DEBUG"
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
echo "Built $SCEN_DIR/{client,server} against $PREFIX (zenoh-pico logging=$WANT_DEBUG)"
