#!/usr/bin/env bash
# Builds one CycloneDDS HIL scenario's client+server (examples/perf_hil/cyclonedds/<scenario>/),
# generating the shared Bench binding locally (once, reused by every scenario) via this host's own
# idlc - deliberately not committed to git (comparison.md's own design note): idlc's generated code
# is tied to the installed CycloneDDS version, which differs between hosts (a real, confirmed gap
# between this repo's dev box and the tickle-hil rpis) - generating fresh on whichever host builds
# is what actually stays correct everywhere, not a committed snapshot from one specific host.
set -euo pipefail

SCENARIO="${1:?usage: build.sh <scenario>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEN_DIR="$HERE/generated"

mkdir -p "$GEN_DIR"
if [ ! -f "$GEN_DIR/Bench.c" ]; then
    idlc -o "$GEN_DIR" "$HERE/../idl/Bench.idl"
fi

SCEN_DIR="$HERE/$SCENARIO"
if [ ! -d "$SCEN_DIR" ]; then
    echo "No such scenario dir: $SCEN_DIR" >&2
    exit 1
fi

# Located rather than hardcoded - the include/lib layout differs between hosts (ROS distro name,
# arch-triplet lib dir), same "generate/build fresh per host" reasoning as Bench.c/.h above.
CDDS_INCLUDE="$(find /opt/ros/*/include -maxdepth 1 -iname CycloneDDS 2>/dev/null | head -1)"
CDDS_LIB="$(find /opt/ros/*/lib/*/  -maxdepth 1 -iname 'libddsc.so*' 2>/dev/null | head -1 | xargs -r dirname)"
if [ -z "$CDDS_INCLUDE" ] || [ -z "$CDDS_LIB" ]; then
    echo "Could not locate an installed CycloneDDS (include: '$CDDS_INCLUDE', lib: '$CDDS_LIB')" >&2
    exit 1
fi

CC="${CC:-gcc}"
CFLAGS="-O2 -I$GEN_DIR -I$CDDS_INCLUDE"
# --disable-new-dtags: DT_RPATH (old-style, transitively searched by every library this binary
# loads, including libddsc.so's own dependency on libiceoryx_binding_c.so) instead of the
# linker's modern default DT_RUNPATH, which only covers this executable's own *direct*
# dependencies - found the hard way (repeat runs failing to find libiceoryx_binding_c.so unless
# /opt/ros/*/setup.bash was sourced first in that exact shell, easy to forget) - this way the
# binary is self-contained regardless of whether the invoking shell sourced anything.
LDFLAGS="-L$CDDS_LIB -Wl,-rpath,$CDDS_LIB -Wl,--disable-new-dtags -lddsc -lm"

# shellcheck disable=SC2086 # CFLAGS/LDFLAGS are deliberately word-split into multiple flags -
# same convention .github/scripts/run_perf.sh/test.sh already use for the identical case.
$CC $CFLAGS -o "$SCEN_DIR/server" "$SCEN_DIR/server.c" "$GEN_DIR/Bench.c" $LDFLAGS
# shellcheck disable=SC2086
$CC $CFLAGS -o "$SCEN_DIR/client" "$SCEN_DIR/client.c" "$GEN_DIR/Bench.c" $LDFLAGS
echo "Built $SCEN_DIR/{client,server}"
