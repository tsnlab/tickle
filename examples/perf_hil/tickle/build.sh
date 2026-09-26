#!/usr/bin/env bash
# Builds one TickLE-native HIL scenario's client+server (examples/perf_hil/tickle/<scenario>/).
# Uses `make install` to a scratch prefix + pkg-config rather than TickLE's own top-level
# Makefile/examples build - deliberately: this whole examples/perf_hil/ tree is "TickLE Plan"'s
# own observation/comparison work (rmw_tickle/COMPARISON.md), kept independent of whatever
# TickLE Dev's own queue is doing to platform/linux/Makefile or the example set it manages.
set -euo pipefail

SCENARIO="${1:?usage: build.sh <scenario> [p1|p2|p3|p4]}"
# Optional payload shape (examples/perf_hil/OPTIMIZATION_PLAN.md section 3). Omitted is today's
# behaviour exactly - the P1 shape, built into <scenario>/ - so every sweep written before the
# campaign keeps working untouched. Given explicitly, the binaries go to <scenario>_<pN>/ instead,
# which is all run_scenario.sh needs to reach them (it forwards the string into REMOTE_DIR).
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
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
SHAPE_DIR="$HERE/common/$SHAPE"
INSTALL_PREFIX="$HOME/tickle_local_install"

# P4 is 2800 bytes, so its sample does not fit the 1472-byte default datagram. Two ways to carry it,
# chosen by TICKLE_P4_PATH, and printed on the RESULT line as sample_path= so rows from the two are
# never compared as if they were one:
#   frag   (the default) - DATA_FRAG: tt_MAX_SAMPLE_LENGTH=4096 with the 1472-byte datagram kept, so
#          core sends two fragments of its own (rmw_tickle/DATAFRAG_PLAN.md).
#   ipfrag - the build before DATA_FRAG: tt_MAX_BUFFER_LENGTH=4096, one 2828-byte datagram that the
#          OS splits into IP fragments. Kept so the change can be measured against what it replaced.
# Either flag sizes libtickle.a's own buffers as well as the example's, so each gets its own prefix -
# same reasoning as TICKLE_RELIABLE_STATS below, and the same remedy. Their node buffers are larger
# than the other shapes' (~10 KB for ipfrag, ~36 KB of reassembly slots for frag), immaterial against
# a peak RSS in the megabytes but the reason the P4 memory figure is not strictly comparable to P1-P3's.
CORE_DEFINE=""
SAMPLE_PATH=datagram
DATAGRAM_BYTES=1472 # what tt_MAX_BUFFER_LENGTH resolves to; the fit check below proves it per build
if [ "$SHAPE" = "p4" ]; then
    case "${TICKLE_P4_PATH:-frag}" in
    frag)
        CORE_DEFINE="-Dtt_MAX_SAMPLE_LENGTH=4096"
        INSTALL_PREFIX="$HOME/tickle_local_install_sample4096"
        SAMPLE_PATH=frag
        # TICKLE_FRAG_SLOTS=N: the receiver's reassembly pool (tt_FRAG_REASSEMBLY_SLOTS, default 8), for the
        # slot-count dose arms. Its own prefix, as for every define that changes libtickle.a; the server's
        # RESULT line reports frag_slots= so a row cannot be mistaken for another arm's.
        if [ -n "${TICKLE_FRAG_SLOTS:-}" ]; then
            CORE_DEFINE="$CORE_DEFINE -Dtt_FRAG_REASSEMBLY_SLOTS=$TICKLE_FRAG_SLOTS"
            INSTALL_PREFIX="${INSTALL_PREFIX}_slots${TICKLE_FRAG_SLOTS}"
        fi
        # TICKLE_DATAGRAM_BYTES=N (2026-09-26): a smaller datagram, so the same 2800 B sample travels as
        # more fragments - 800 gives exactly 4 (771 B in FRAG_FIRST, 782 B in each FRAG_CONT). A
        # TickLE-only diagnostic for how recovery cost scales with fragment count, used to measure the
        # per-datagram seq_no change (rmw_tickle/DATAFRAG_PLAN.md section 13.3) without adding a fifth
        # payload shape to every framework. Its own prefix; RESULT lines say datagram_bytes=.
        if [ -n "${TICKLE_DATAGRAM_BYTES:-}" ]; then
            CORE_DEFINE="$CORE_DEFINE -Dtt_MAX_BUFFER_LENGTH=$TICKLE_DATAGRAM_BYTES"
            INSTALL_PREFIX="${INSTALL_PREFIX}_dgram${TICKLE_DATAGRAM_BYTES}"
            DATAGRAM_BYTES="$TICKLE_DATAGRAM_BYTES"
        fi
        ;;
    ipfrag)
        # tt_FRAG_ENABLED=0 explicitly: a buffer larger than the control datagram now compiles
        # fragmentation in by itself (config.h), which is rmw_tickle's configuration and not this arm's.
        CORE_DEFINE="-Dtt_MAX_BUFFER_LENGTH=4096 -Dtt_FRAG_ENABLED=0"
        INSTALL_PREFIX="$HOME/tickle_local_install_buf4096"
        SAMPLE_PATH=ipfrag
        DATAGRAM_BYTES=4096
        ;;
    *)
        echo "TICKLE_P4_PATH must be frag (the default, DATA_FRAG) or ipfrag (one datagram, split by the OS)" >&2
        exit 1
        ;;
    esac
fi

# TICKLE_RELIABLE_STATS=1: build libtickle.a *and* the example with -Dtt_RELIABLE_STATS
# (include/tickle/reliable_stats.h - measurement-only RELIABLE recovery counters, printed as
# "RSTATS:" lines at the end of reliable_throughput's own run). Installed to its own prefix, and
# always rebuilt from scratch, so it can never be mistaken for (or reuse) the plain build above.
STATS_DEFINE=""
if [ "${TICKLE_RELIABLE_STATS:-0}" = "1" ]; then
    INSTALL_PREFIX="${INSTALL_PREFIX}_rstats"
    STATS_DEFINE="-Dtt_RELIABLE_STATS"
fi

# TICKLE_RX_BATCH=N (2026-09-26): tt_RX_BATCH, the datagrams one recvmmsg() reads (hal_linux.h), for the
# arms that size it. Own prefix, as for every define that changes libtickle.a; the reliable_throughput
# server's RESULT line reports rx_batch= and how full the batches ran.
if [ -n "${TICKLE_RX_BATCH:-}" ]; then
    INSTALL_PREFIX="${INSTALL_PREFIX}_rxbatch${TICKLE_RX_BATCH}"
    STATS_DEFINE="${STATS_DEFINE:-} -Dtt_RX_BATCH=$TICKLE_RX_BATCH"
fi

# TICKLE_THREAD_UNSAFE=1 builds libtickle.a and the example with -Dtt_THREAD_SAFE=0, i.e. with the
# locking compiled out entirely. It exists for one measurement: the M1 re-run showed core's locks
# costing +250 ns a sample on the rig while TickLE Dev measured the same code at +12 ns on x86, and
# neither the lock-pair cost (8-12 ns here) nor the acquisition count (~1.1 a sample) explains 250.
# Flipping this flag against the same commit is the strongest A/B available - no checkout, no other
# code difference - so it isolates whether the cost is the THREAD_SAFE path at all. Own prefix, for
# the same reason as the flags below: it changes how libtickle.a itself is compiled.
if [ "${TICKLE_THREAD_UNSAFE:-0}" = "1" ]; then
    INSTALL_PREFIX="${INSTALL_PREFIX}_nothread"
    STATS_DEFINE="${STATS_DEFINE:-} -Dtt_THREAD_SAFE=0"
fi

# TICKLE_DYNAMIC_RETRY picks the ACKNACK retry interval explicitly, in both directions: 1 builds with
# tt_RELIABLE_RETRY_INTERVAL 0 (dynamic), 0 builds with the old fixed 1ms. Unset takes config.h's
# default, which is dynamic since 2026-09-25 (the user's decision). It used to be that 0 and unset
# were the same thing - the fixed default - and the A/B scripts pass 0 to mean "the fixed arm", so
# when the default flipped, 0 had to become explicit or those scripts would have compared dynamic
# against dynamic and reported no difference. Each explicit mode gets its own prefix for the same
# reason as the two above: it changes how libtickle.a itself is compiled. The server's RESULT line
# reports which mode it ran (retry_interval_cfg_ns=) and the estimate it learned, so a cell says what
# it measured rather than relying on whoever built it remembering.
case "${TICKLE_DYNAMIC_RETRY:-}" in
1)
    INSTALL_PREFIX="${INSTALL_PREFIX}_dynretry"
    STATS_DEFINE="$STATS_DEFINE -Dtt_RELIABLE_RETRY_INTERVAL=0"
    ;;
0)
    INSTALL_PREFIX="${INSTALL_PREFIX}_fixedretry"
    STATS_DEFINE="$STATS_DEFINE -Dtt_RELIABLE_RETRY_INTERVAL=1000000"
    ;;
"") ;;
*)
    echo "TICKLE_DYNAMIC_RETRY must be 0 (fixed 1ms), 1 (dynamic) or unset (config.h's default)" >&2
    exit 1
    ;;
esac

# TICKLE_CORE_BUILD: how libtickle.a itself is optimised - release (-O2, the default since 2026-09-26)
# or debug (-O0 -g). Until then this script built the core through `make install` with the platform
# Makefile's own default, BUILD_TYPE=debug, so every TickLE figure measured on the rig - the campaign,
# the single-packet sweep, every COMPARISON.md cell - was an unoptimised core against CycloneDDS and
# FastDDS release packages. Only this directory's client.c/server.c were -O2. Found by disassembling two
# rig builds: not a single static helper was inlined. `debug` reproduces those builds exactly, for a
# like-for-like comparison with anything measured before; the RESULT line says which one ran
# (core_build=). Release gets its own prefix, because the reinstall check below only looks at source
# times, and an existing -O0 prefix would otherwise have survived this fix untouched.
case "${TICKLE_CORE_BUILD:-release}" in
release)
    CORE_BUILD_TYPE=release
    INSTALL_PREFIX="${INSTALL_PREFIX}_o2"
    ;;
debug)
    CORE_BUILD_TYPE=debug
    ;;
*)
    echo "TICKLE_CORE_BUILD must be release (the default, -O2) or debug (-O0, the builds before 2026-09-26)" >&2
    exit 1
    ;;
esac

# Anything that changes how libtickle.a itself is compiled gets a from-scratch build into its own
# prefix, bracketed by `make clean`. Not belt and braces: object files in the repo build dir are
# reused across `make install` calls regardless of CPPFLAGS, so without the clean a prefix named
# for one setting can be filled with objects compiled under another - a silently wrong measurement
# rather than a build failure.
CORE_DEFINES="$CORE_DEFINE $STATS_DEFINE"

# Reinstall when the prefix is missing *or* older than any core source, not just when it's missing
# (2026-09-23, a real trap: a prefix left from an earlier day silently built the example against a
# stale libtickle.a, failing on a struct member that a later commit had added - the same staleness
# lesson run_perf.sh's own `rm -rf` of the prefix already encodes for the rpis).
NEEDS_INSTALL=0
if [ ! -f "$INSTALL_PREFIX/lib/libtickle.a" ]; then
    NEEDS_INSTALL=1
elif [ -n "$(find "$REPO_ROOT/src" "$REPO_ROOT/include" -type f -newer "$INSTALL_PREFIX/lib/libtickle.a" -print -quit)" ]; then
    echo "libtickle.a in $INSTALL_PREFIX is older than src/ or include/ - reinstalling"
    NEEDS_INSTALL=1
fi
# Always bracketed by `make clean` now, defines or not: every BUILD_TYPE links the same
# platform/linux/libtickle.a, and make judges it by timestamps, so an archive left by a build of the
# other type could pass for up to date and be installed under the wrong prefix.
if [ "$NEEDS_INSTALL" = "1" ]; then
    (cd "$REPO_ROOT" && make clean && make install "PREFIX=$INSTALL_PREFIX" "BUILD_TYPE=$CORE_BUILD_TYPE" \
        "CPPFLAGS=$CORE_DEFINES" && make clean)
fi

SRC_DIR="$HERE/$SCENARIO"
if [ ! -d "$SRC_DIR" ]; then
    echo "No such scenario dir: $SRC_DIR" >&2
    exit 1
fi
if [ ! -f "$SHAPE_DIR/Bench.c" ]; then
    echo "No generated payload shape at $SHAPE_DIR - run 'make regen' from the repo root" >&2
    exit 1
fi
SCEN_DIR="$SRC_DIR"
if [ -n "$PAYLOAD" ]; then
    SCEN_DIR="$HERE/${SCENARIO}_${PAYLOAD}"
    mkdir -p "$SCEN_DIR"
fi

# The CDR sample size, derived from the shape's own source rather than written down a second
# time: 8 (send_ns) + 4 (seq) + the payload array. Every harness reports it in its RESULT line so
# the payload-boundary gate is checkable from the line alone. The probe below asserts it against
# the generator's own sizeof, so a wrong array-size parse here is a build failure, not a wrong
# number in the results.
BENCH_ARRAY="$(sed -n 's/^uint8\[\([0-9]*\)\] *payload.*/\1/p' "$SHAPE_DIR/Bench.msg")"
if [ -z "$BENCH_ARRAY" ]; then
    echo "Could not read the payload array size out of $SHAPE_DIR/Bench.msg" >&2
    exit 1
fi
BENCH_SAMPLE_BYTES=$((12 + BENCH_ARRAY))

CC="${CC:-gcc}"
PKG_CONFIG_PATH="$INSTALL_PREFIX/lib/pkgconfig"
TICKLE_CFLAGS="$(PKG_CONFIG_PATH="$PKG_CONFIG_PATH" pkg-config --cflags tickle)"
TICKLE_LIBS="$(PKG_CONFIG_PATH="$PKG_CONFIG_PATH" pkg-config --libs tickle)"
# The payload shape comes first on the include path, and the four shapes all declare the same
# `struct BenchData` / `BenchTopic`, so every scenario's own client.c and server.c compiles
# unchanged at each size - the size is chosen here and nowhere else.
CFLAGS="-O2 -DBENCH_CORE_BUILD=$CORE_BUILD_TYPE -DBENCH_SAMPLE_PATH=$SAMPLE_PATH -DBENCH_DATAGRAM_BYTES=$DATAGRAM_BYTES $CORE_DEFINE $STATS_DEFINE -DBENCH_SAMPLE_BYTES=$BENCH_SAMPLE_BYTES -I$SHAPE_DIR -I$HERE/common $TICKLE_CFLAGS"

# Compile-time proof that this shape can be sent by the build *this* libtickle.a was compiled as, and
# by the path sample_path= will claim. The generator emits BenchData_FITS_ONE_DATAGRAM as
# (sample_bytes <= tt_MAX_BUFFER_LENGTH), and both limits come from the installed prefix's own
# tickle/config.h - so building p4 against the wrong prefix fails here instead of producing a silently
# wrong measurement, which is the one failure mode of having several prefixes. frag must really
# fragment (not fit one datagram) and must fit tt_MAX_SAMPLE_LENGTH; every other path must fit one
# datagram. Checked as its own translation unit so the message names the shape.
if [ "$SAMPLE_PATH" = "frag" ]; then
    FIT_CHECK='_Static_assert(tt_FRAG_ENABLED && !BenchData_FITS_ONE_DATAGRAM && BENCH_SAMPLE_BYTES <= tt_MAX_SAMPLE_LENGTH, "sample_path=frag but this build would not fragment this shape");'
else
    FIT_CHECK='_Static_assert(BenchData_FITS_ONE_DATAGRAM, "payload shape does not fit this build tt_MAX_BUFFER_LENGTH");'
fi
# shellcheck disable=SC2086
printf '#include "Bench.h"\n%s\n_Static_assert(BENCH_SAMPLE_BYTES == sizeof(struct BenchData), "BENCH_SAMPLE_BYTES disagrees with the generated struct");\n_Static_assert(BENCH_DATAGRAM_BYTES == tt_MAX_BUFFER_LENGTH, "datagram_bytes= would misreport this build");\n' "$FIT_CHECK" |
    $CC $CFLAGS -fsyntax-only -x c - || {
    echo "Payload shape $SHAPE cannot be sent as sample_path=$SAMPLE_PATH by $INSTALL_PREFIX" >&2
    exit 1
}

# shellcheck disable=SC2086 # TICKLE_CFLAGS/TICKLE_LIBS are deliberately word-split - see
# cyclonedds/build.sh's own identical note.
$CC $CFLAGS -o "$SCEN_DIR/server" "$SRC_DIR/server.c" "$SHAPE_DIR/Bench.c" $TICKLE_LIBS -lm -lpthread
# shellcheck disable=SC2086
$CC $CFLAGS -o "$SCEN_DIR/client" "$SRC_DIR/client.c" "$SHAPE_DIR/Bench.c" $TICKLE_LIBS -lm -lpthread
echo "Built $SCEN_DIR/{client,server}"
