#!/usr/bin/env bash
# Builds one FastDDS HIL scenario's client+server (examples/perf_hil/fastdds/<scenario>/),
# generating the shared Bench binding fresh via this host's own fastddsgen - same "generate per
# host, don't commit" reasoning as cyclonedds/build.sh (a real, confirmed version gap: this repo's
# dev box has FastDDS 3.6.2/fastddsgen 4.0.3 (v3-API-only output), the tickle-hil rpis have
# FastDDS 2.14.6/fastddsgen 2.3.0+dfsg (v2-API output) - the two are not interchangeable.
set -euo pipefail

SCENARIO="${1:?usage: build.sh <scenario>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEN_DIR="$HERE/generated"

mkdir -p "$GEN_DIR"
if [ ! -f "$GEN_DIR/Bench.cxx" ]; then
    fastddsgen -replace -d "$GEN_DIR" "$HERE/../idl/Bench.idl"

    # A real, confirmed API drift between this Ubuntu-packaged fastddsgen (2.3.0+dfsg) and the
    # actually-installed fastcdr (2.2.7, via ros-*-fastcdr): fastddsgen's own generated code calls
    # the pre-rename camelCase Cdr methods and references Cdr::DDS_CDR as a nested enumerator -
    # both moved (get_serialized_data_length(), and a free eprosima::fastcdr::DDS_CDR enum) in the
    # installed version. Found by actually compiling and reading the real errors, not assumed -
    # every generated *PubSubTypes.cxx this exercise produces needs the same two fixes, so patch
    # it here once rather than by hand per scenario.
    sed -i \
        -e 's/eprosima::fastcdr::Cdr::DDS_CDR/eprosima::fastcdr::DDS_CDR/g' \
        -e 's/\.getSerializedDataLength()/.get_serialized_data_length()/g' \
        "$GEN_DIR"/*PubSubTypes.cxx

    # The generated header's own GEN_API_VER==1 guard blocks compilation against this install's
    # actual TopicDataType (whose own config.h defines GEN_API_VER=2) - checked the real installed
    # fastdds/dds/topic/TopicDataType.hpp directly before bypassing this: every pure-virtual method
    # it actually requires (serialize/deserialize/getSerializedSizeProvider/createData/deleteData/
    # getKey, all with the same signatures) is exactly what fastddsgen already generated below -
    # the version tags disagree, but the real interface contract does not, so bypassing this
    # specific check (not the two real API-rename fixes above) is safe here.
    sed -i '/#if !defined(GEN_API_VER)/,/#endif  \/\/ GEN_API_VER/d' "$GEN_DIR"/*PubSubTypes.h
fi

SCEN_DIR="$HERE/$SCENARIO"
if [ ! -d "$SCEN_DIR" ]; then
    echo "No such scenario dir: $SCEN_DIR" >&2
    exit 1
fi

# v2.x (the tickle-hil rpis' own installed FastDDS/fastddsgen line) names both the package and
# the library "fastrtps", not "fastdds" - found by actually listing what's installed, not assumed
# from the v3-era "Fast DDS" rename.
FDDS_INCLUDE="$(find /opt/ros/*/include -maxdepth 1 -iname fastrtps 2>/dev/null | head -1)"
FDDS_LIB="$(find /opt/ros/*/lib -iname 'libfastrtps.so*' 2>/dev/null | head -1 | xargs -r dirname)"
# Not dirname'd like FDDS_INCLUDE above - this install nests an extra "fastcdr/" level
# (.../include/fastcdr/fastcdr/Cdr.h), so the search root #include <fastcdr/Cdr.h> needs is the
# found directory itself, not its parent - found by listing the actual tree, not assumed.
FASTCDR_INCLUDE="$(find /opt/ros/*/include -maxdepth 1 -iname fastcdr 2>/dev/null | head -1)"
if [ -z "$FDDS_INCLUDE" ] || [ -z "$FDDS_LIB" ] || [ -z "$FASTCDR_INCLUDE" ]; then
    echo "Could not locate an installed FastDDS/FastRTPS/FastCDR (include: '$FDDS_INCLUDE', lib: '$FDDS_LIB', fastcdr: '$FASTCDR_INCLUDE')" >&2
    exit 1
fi

CXX="${CXX:-g++}"
CXXFLAGS="-O2 -std=c++17 -I$GEN_DIR -I$FDDS_INCLUDE -I$FASTCDR_INCLUDE"
LDFLAGS="-L$FDDS_LIB -lfastrtps -lfastcdr -lpthread"

$CXX $CXXFLAGS -o "$SCEN_DIR/server" "$SCEN_DIR/server.cpp" "$GEN_DIR/Bench.cxx" "$GEN_DIR/BenchPubSubTypes.cxx" $LDFLAGS
$CXX $CXXFLAGS -o "$SCEN_DIR/client" "$SCEN_DIR/client.cpp" "$GEN_DIR/Bench.cxx" "$GEN_DIR/BenchPubSubTypes.cxx" $LDFLAGS
echo "Built $SCEN_DIR/{client,server}"
echo "Run with: LD_LIBRARY_PATH=$FDDS_LIB ./client|./server"
