#!/usr/bin/env bash
# Builds one TickLE-native HIL scenario's client+server (examples/perf_hil/tickle/<scenario>/).
# Uses `make install` to a scratch prefix + pkg-config rather than TickLE's own top-level
# Makefile/examples build - deliberately: this whole examples/perf_hil/ tree is "TickLE Plan"'s
# own observation/comparison work (rmw_tickle/comparison.md), kept independent of whatever
# TickLE Dev's own queue is doing to platform/linux/Makefile or the example set it manages.
set -euo pipefail

SCENARIO="${1:?usage: build.sh <scenario>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
INSTALL_PREFIX="$HOME/tickle_local_install"

if [ ! -f "$INSTALL_PREFIX/lib/libtickle.a" ]; then
    (cd "$REPO_ROOT" && make install "PREFIX=$INSTALL_PREFIX")
fi

SCEN_DIR="$HERE/$SCENARIO"
if [ ! -d "$SCEN_DIR" ]; then
    echo "No such scenario dir: $SCEN_DIR" >&2
    exit 1
fi

CC="${CC:-gcc}"
PKG_CONFIG_PATH="$INSTALL_PREFIX/lib/pkgconfig"
TICKLE_CFLAGS="$(PKG_CONFIG_PATH="$PKG_CONFIG_PATH" pkg-config --cflags tickle)"
TICKLE_LIBS="$(PKG_CONFIG_PATH="$PKG_CONFIG_PATH" pkg-config --libs tickle)"
CFLAGS="-O2 -I$HERE/common $TICKLE_CFLAGS"

# shellcheck disable=SC2086 # TICKLE_CFLAGS/TICKLE_LIBS are deliberately word-split - see
# cyclonedds/build.sh's own identical note.
$CC $CFLAGS -o "$SCEN_DIR/server" "$SCEN_DIR/server.c" "$HERE/common/Bench.c" $TICKLE_LIBS -lm -lpthread
# shellcheck disable=SC2086
$CC $CFLAGS -o "$SCEN_DIR/client" "$SCEN_DIR/client.c" "$HERE/common/Bench.c" $TICKLE_LIBS -lm -lpthread
echo "Built $SCEN_DIR/{client,server}"
