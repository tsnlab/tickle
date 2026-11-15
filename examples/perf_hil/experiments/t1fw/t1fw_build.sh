#!/usr/bin/env bash
# Builds every binary of the T1 framework comparison into $BIN (default $HOME/tmp_build/t1fw_v214/bin): TickLE (node_d100,
# large_sample_bench via t1/t1_build_node.sh), the CycloneDDS 0.10.5 and Fast DDS 2.14.6 nodes (the rig's versions, built by
# t1fw_build_deps.sh under $T1FW_DEPS/prefix) and the zenoh-pico 1.10.2 node. Node sources: examples/perf_hil/t1fw_vendor/
# (not TickLE product code, not a lint target). Take `flock /tmp/gates.lock` around it: builds must not overlap a measurement.
# Simulation instrument; no library code changed.
set -euo pipefail
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/../../../.." && pwd)
V=$REPO/examples/perf_hil/t1fw_vendor
BIN=${BIN:-$HOME/tmp_build/t1fw_v214/bin}
DEPS=${T1FW_DEPS:-$HOME/t1fw_v214}
P=$DEPS/prefix
LIBS=$P/lib
ZP_PREFIX=${ZENOH_PICO_PREFIX:-$HOME/zenohpico_install_1.10.2}
ZP_SRC=${ZENOH_PICO_SRC:-$HOME/zenohpico_src_1.10.2}
ZP_COMMIT=6a881c21dad1f778d0529eb9f297bea339016526
ZP_OPTS=${ZENOH_CMAKE_OPTS:-}
mkdir -p "$BIN/gen_cyclone" "$BIN/gen_fastdds"
export LD_LIBRARY_PATH=$LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
[ -x "$P/bin/idlc" ] || { echo "no $P/bin/idlc: run t1fw_build_deps.sh first"; exit 1; }
FASTDDSGEN=${FASTDDSGEN:-$P/bin/fastddsgen}
[ -x "$FASTDDSGEN" ] || { echo "no $FASTDDSGEN: run t1fw_build_deps.sh first"; exit 1; }

# TickLE: the T1 nodes (depth-100 ping/echo node, large_sample_bench) from this tree
T1_DIR=$BIN "$REPO/examples/perf_hil/experiments/t1/t1_build_node.sh" "$BIN"

# CycloneDDS 0.10.5
(cd "$BIN/gen_cyclone" && cp "$V/Blob.idl" . && "$P/bin/idlc" -o . Blob.idl)
cc -O2 -I"$V" -I"$BIN/gen_cyclone" -I"$P/include" -o "$BIN/cyclonedds_node" "$V/cyclonedds_node.c" "$BIN/gen_cyclone/Blob.c" \
    -L"$LIBS" -lddsc -Wl,--disable-new-dtags -Wl,-rpath,"$LIBS"
echo "built cyclonedds_node (CycloneDDS 0.10.5)"

# Fast DDS 2.14.6, generated binding patched as examples/perf_hil/fastdds/build.sh does (two API renames, the GEN_API_VER guard)
(cd "$BIN/gen_fastdds" && cp "$V/Blob.idl" . && "$FASTDDSGEN" -replace -d . Blob.idl >gen.log 2>&1)
sed -i -e 's/eprosima::fastcdr::Cdr::DDS_CDR/eprosima::fastcdr::DDS_CDR/g' -e 's/\.getSerializedDataLength()/.get_serialized_data_length()/g' "$BIN"/gen_fastdds/*PubSubTypes.cxx
sed -i '/#if !defined(GEN_API_VER)/,/#endif  \/\/ GEN_API_VER/d' "$BIN"/gen_fastdds/*PubSubTypes.h
g++ -O2 -std=c++17 -I"$V" -I"$BIN/gen_fastdds" -I"$P/include" -o "$BIN/fastdds_node" "$V/fastdds_node.cpp" \
    "$BIN/gen_fastdds/Blob.cxx" "$BIN/gen_fastdds/BlobPubSubTypes.cxx" -L"$LIBS" -Wl,--disable-new-dtags -Wl,-rpath,"$LIBS" -lfastrtps -lfastcdr -lpthread
echo "built fastdds_node (Fast DDS 2.14.6)"

# zenoh-pico (pinned tag 1.10.2, as examples/perf_hil/zenohpico/build.sh)
if [ ! -f "$ZP_PREFIX/lib/libzenohpico.so" ]; then
    rm -rf "$ZP_SRC"
    git clone -q --depth 1 --branch 1.10.2 https://github.com/eclipse-zenoh/zenoh-pico.git "$ZP_SRC"
    [ "$(git -C "$ZP_SRC" rev-parse HEAD)" = "$ZP_COMMIT" ] || { echo "REFUSING: 1.10.2 resolved to a different commit"; exit 1; }
    # shellcheck disable=SC2086
    cmake -S "$ZP_SRC" -B "$ZP_SRC/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ZP_PREFIX" $ZP_OPTS >/dev/null
    cmake --build "$ZP_SRC/build" --parallel "$(nproc)" >/dev/null
    cmake --install "$ZP_SRC/build" >/dev/null
fi
cc -O2 -DZENOH_LINUX=1 -I"$V" -I"$ZP_PREFIX/include" -o "$BIN/zenohpico_node" "$V/zenohpico_node.c" -L"$ZP_PREFIX/lib" -lzenohpico -lpthread -lm \
    -Wl,-rpath,"$ZP_PREFIX/lib"
echo "built zenohpico_node (zenoh-pico $ZP_COMMIT)"
echo "BUILD_OK $BIN"
