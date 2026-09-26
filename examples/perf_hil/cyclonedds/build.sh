#!/usr/bin/env bash
# Builds one CycloneDDS HIL scenario's client+server (examples/perf_hil/cyclonedds/<scenario>/),
# generating the shared Bench binding locally (once, reused by every scenario) via this host's own
# idlc - deliberately not committed to git (COMPARISON.md's own design note): idlc's generated code
# is tied to the installed CycloneDDS version, which differs between hosts (a real, confirmed gap
# between this repo's dev box and the tickle-hil rpis) - generating fresh on whichever host builds
# is what actually stays correct everywhere, not a committed snapshot from one specific host.
set -euo pipefail

SCENARIO="${1:?usage: build.sh <scenario> [p1|p2|p3|p4]}"
# Optional payload shape (examples/perf_hil/OPTIMIZATION_PLAN.md section 3). Omitted is today's
# behaviour exactly - the P1 shape, built into <scenario>/ - so every sweep written before the
# campaign keeps working untouched. Given explicitly, the binaries go to <scenario>_<pN>/ instead,
# which is all run_scenario.sh needs to reach them (it forwards the string into REMOTE_DIR).
# All four idl/pN/Bench.idl declare the same `struct Bench`, so the scenario sources compile
# unchanged at every size and the size is chosen by which directory the generator is pointed at.
PAYLOAD="${2:-}"
SHAPE="${PAYLOAD:-p1}"
case "$SHAPE" in
p1 | p2 | p3 | p4) ;;
*)
    echo "Unknown payload shape: $SHAPE (want p1, p2, p3 or p4)" >&2
    exit 1
    ;;
esac
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEN_DIR="$HERE/generated/$SHAPE"

IDL="$HERE/../idl/$SHAPE/Bench.idl"
# shellcheck disable=SC1091 # sibling helper, sourced by path relative to this script
. "$HERE/../bench_shape.sh"

# The CDR sample size, derived from this shape's own IDL rather than written down a second time:
# 8 (send_ns) + 4 (seq) + the payload array. Reported in the RESULT line by every harness so the
# payload-boundary gate is checkable from the line alone - which is only true if the binding below
# was generated from this same IDL, and that is checked below rather than assumed.
BENCH_ARRAY="$(bench_idl_payload "$IDL")"
if [ -z "$BENCH_ARRAY" ]; then
    echo "Could not read the payload array size out of $IDL" >&2
    exit 1
fi
BENCH_SAMPLE_BYTES=$((12 + BENCH_ARRAY))

# Regenerated whenever the binding is missing, older than its IDL, or declares a different payload
# size (2026-09-25). It used to be regenerated only when missing, and generated/ is gitignored, so a
# binding from before a resize survived every checkout and build since - see ../bench_shape.sh.
# The size check is the one that does not depend on file times, which a checkout or a copy can set.
mkdir -p "$GEN_DIR"
if [ ! -f "$GEN_DIR/Bench.c" ] || [ ! -f "$GEN_DIR/Bench.h" ] || [ "$IDL" -nt "$GEN_DIR/Bench.c" ] ||
    [ "$(bench_cyclonedds_gen_payload "$GEN_DIR/Bench.h")" != "$BENCH_ARRAY" ]; then
    rm -f "$GEN_DIR"/Bench*
    if [ -x /opt/ros/rolling/bin/idlc ]; then
        IDLC=/opt/ros/rolling/bin/idlc
        IDLC_LIB_DIR="$(find /opt/ros/rolling/lib/*/ -maxdepth 1 -iname 'libddsc.so*' 2>/dev/null | head -1 | xargs -r dirname)"
    else
        IDLC="$(command -v idlc || true)"
        IDLC_LIB_DIR="$(find /opt/ros/*/lib/*/  -maxdepth 1 -iname 'libddsc.so*' 2>/dev/null | head -1 | xargs -r dirname)"
    fi
    # Named rather than left to `set -e` to end the script wordlessly: this is the one failure a
    # host without CycloneDDS installed hits, and inside a 45-minute sweep a silent exit 1 is
    # indistinguishable from a build that produced nothing for some subtler reason.
    if [ -z "$IDLC" ]; then
        echo "No idlc on this host - CycloneDDS's IDL compiler is needed to build this harness" >&2
        exit 1
    fi
    LD_LIBRARY_PATH="$IDLC_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$IDLC" -o "$GEN_DIR" "$IDL"
fi

# Whatever path got here, the binding about to be compiled must be this IDL's. A refusal here costs
# one rebuild; a mismatch that got through cost a whole P2 column.
GEN_ARRAY="$(bench_cyclonedds_gen_payload "$GEN_DIR/Bench.h")"
if [ "$GEN_ARRAY" != "$BENCH_ARRAY" ]; then
    echo "Generated $GEN_DIR/Bench.h declares payload[$GEN_ARRAY] but $IDL declares payload[$BENCH_ARRAY] - refusing to build" >&2
    exit 1
fi

SRC_DIR="$HERE/$SCENARIO"
if [ ! -d "$SRC_DIR" ]; then
    echo "No such scenario dir: $SRC_DIR" >&2
    exit 1
fi
SCEN_DIR="$SRC_DIR"
if [ -n "$PAYLOAD" ]; then
    SCEN_DIR="$HERE/${SCENARIO}_${PAYLOAD}"
    mkdir -p "$SCEN_DIR"
fi

# Located rather than hardcoded - the include/lib layout differs between hosts (ROS distro name,
# arch-triplet lib dir), same "generate/build fresh per host" reasoning as Bench.c/.h above.
# Prefers /opt/ros/rolling if present - a real, confirmed CycloneDDS discovery bug on the rig's
# own default (jazzy's 0.10.5) never matches on any topic name other than the two latency
# scenarios' own "ping"/"pong" (COMPARISON.md's own "Blocked" section) - rolling's own CycloneDDS
# (11.0.1, matching this repo's dev box) installed alongside jazzy specifically to test whether a
# newer release doesn't have the same defect, per that section's own recommendation.
#
# `|| true` on the rolling lookups only (2026-09-25): on a host with no rolling install, `find`
# fails on the missing directory, and under pipefail that ended this script silently before the
# fallback below could run. Absence is decided by the explicit -z checks, which say why.
CDDS_INCLUDE="$({ find /opt/ros/rolling/include -maxdepth 1 -iname CycloneDDS 2>/dev/null || true; } | head -1)"
CDDS_LIB="$({ find /opt/ros/rolling/lib/*/ -maxdepth 1 -iname 'libddsc.so*' 2>/dev/null || true; } | head -1 | xargs -r dirname)"
if [ -z "$CDDS_INCLUDE" ] || [ -z "$CDDS_LIB" ]; then
    CDDS_INCLUDE="$(find /opt/ros/*/include -maxdepth 1 -iname CycloneDDS 2>/dev/null | head -1)"
    CDDS_LIB="$(find /opt/ros/*/lib/*/  -maxdepth 1 -iname 'libddsc.so*' 2>/dev/null | head -1 | xargs -r dirname)"
fi
if [ -z "$CDDS_INCLUDE" ] || [ -z "$CDDS_LIB" ]; then
    echo "Could not locate an installed CycloneDDS (include: '$CDDS_INCLUDE', lib: '$CDDS_LIB')" >&2
    exit 1
fi


CC="${CC:-gcc}"
# -fno-strict-aliasing: a real, bisected bug (not assumed) - the best_effort_throughput scenario's
# server silently received nothing at plain -O2 (reader/writer matched fine, dds_take() just never
# saw the incoming samples), root-caused by rebuilding at -O0 (worked), then -O2 -fno-strict-
# aliasing (also worked) - the DDS C API's own void*-based dds_take()/samples[] pattern is a known
# class of strict-aliasing hazard for a C caller, and this benchmark gains nothing from the
# type-based aliasing optimizations it disables.
CFLAGS="-O2 -DBENCH_SAMPLE_BYTES=$BENCH_SAMPLE_BYTES -fno-strict-aliasing -I$GEN_DIR -I$CDDS_INCLUDE"
# --disable-new-dtags: DT_RPATH (old-style, transitively searched by every library this binary
# loads, including libddsc.so's own dependency on libiceoryx_binding_c.so) instead of the
# linker's modern default DT_RUNPATH, which only covers this executable's own *direct*
# dependencies - found the hard way (repeat runs failing to find libiceoryx_binding_c.so unless
# /opt/ros/*/setup.bash was sourced first in that exact shell, easy to forget) - this way the
# binary is self-contained regardless of whether the invoking shell sourced anything.
LDFLAGS="-L$CDDS_LIB -Wl,-rpath,$CDDS_LIB -Wl,--disable-new-dtags -lddsc -lm"

# shellcheck disable=SC2086 # CFLAGS/LDFLAGS are deliberately word-split into multiple flags -
# same convention .github/scripts/run_perf.sh/test.sh already use for the identical case.
$CC $CFLAGS -o "$SCEN_DIR/server" "$SRC_DIR/server.c" "$GEN_DIR/Bench.c" $LDFLAGS
# shellcheck disable=SC2086
$CC $CFLAGS -o "$SCEN_DIR/client" "$SRC_DIR/client.c" "$GEN_DIR/Bench.c" $LDFLAGS
echo "Built $SCEN_DIR/{client,server}"
