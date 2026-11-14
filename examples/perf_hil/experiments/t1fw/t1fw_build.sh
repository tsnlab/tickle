#!/usr/bin/env bash
# Builds every binary of the T1 framework comparison into $BIN (default $HOME/tmp_build/t1fw/bin): TickLE (node_d100,
# large_sample_bench via t1/t1_build_node.sh), CycloneDDS and Fast DDS nodes (PC: ROS lyrical's 11.0.1 and 3.6), zenoh-pico 1.10.2
# nodes. Take `flock /tmp/gates.lock` around it: builds must not overlap a measurement. Simulation instrument; no library code changed.
set -euo pipefail
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/../../../.." && pwd)
BIN=${BIN:-$HOME/tmp_build/t1fw/bin}
ROS=${ROS:-/opt/ros/lyrical}
FASTDDSGEN=${FASTDDSGEN:-$HOME/tmp_build/ctl/perf/src/performance_test/bin/fastddsgen}
ZP_PREFIX=${ZENOH_PICO_PREFIX:-$HOME/zenohpico_install_1.10.2}
ZP_SRC=${ZENOH_PICO_SRC:-$HOME/zenohpico_src_1.10.2}
ZP_COMMIT=6a881c21dad1f778d0529eb9f297bea339016526
ZP_OPTS=${ZENOH_CMAKE_OPTS:-}
mkdir -p "$BIN/gen_cyclone" "$BIN/gen_fastdds"
export LD_LIBRARY_PATH=$ROS/lib:$ROS/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# TickLE: the T1 nodes (depth-100 ping/echo node, large_sample_bench) from this tree
T1_DIR=$BIN "$REPO/examples/perf_hil/experiments/t1/t1_build_node.sh" "$BIN"

# CycloneDDS
(cd "$BIN/gen_cyclone" && cp "$S/Blob.idl" . && "$ROS/bin/idlc" -o . Blob.idl)
cc -O2 -I"$S" -I"$BIN/gen_cyclone" -I"$ROS/include/CycloneDDS" -o "$BIN/cyclonedds_node" "$S/../../cyclonedds/t1_wifi_node/node.c" "$BIN/gen_cyclone/Blob.c" \
    -L"$ROS/lib/x86_64-linux-gnu" -lddsc -Wl,-rpath,"$ROS/lib/x86_64-linux-gnu"
echo "built cyclonedds_node ($("$ROS/bin/idlc" -v 2>&1 | head -1))"

# Fast DDS
(cd "$BIN/gen_fastdds" && cp "$S/Blob.idl" . && "$FASTDDSGEN" -replace -d . Blob.idl >gen.log 2>&1)
g++ -O2 -std=c++17 -I"$S" -I"$BIN/gen_fastdds" -I"$ROS/include/fastdds" -I"$ROS/includefastcdr" -o "$BIN/fastdds_node" "$S/../../fastdds/t1_wifi_node/node.cpp" \
    "$BIN/gen_fastdds/BlobPubSubTypes.cxx" "$BIN/gen_fastdds"/BlobTypeObjectSupport.cxx -L"$ROS/lib" -L"$ROS/lib/x86_64-linux-gnu" \
    -Wl,-rpath,"$ROS/lib:$ROS/lib/x86_64-linux-gnu" -lfastdds -lfastcdr -lpthread
echo "built fastdds_node"

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
cc -O2 -DZENOH_LINUX=1 -I"$S" -I"$ZP_PREFIX/include" -o "$BIN/zenohpico_node" "$S/../../zenohpico/t1_wifi_node/node.c" -L"$ZP_PREFIX/lib" -lzenohpico -lpthread -lm \
    -Wl,-rpath,"$ZP_PREFIX/lib"
echo "built zenohpico_node (zenoh-pico $ZP_COMMIT)"
echo "BUILD_OK $BIN"
