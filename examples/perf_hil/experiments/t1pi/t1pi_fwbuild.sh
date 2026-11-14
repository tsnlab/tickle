#!/usr/bin/env bash
# Build the framework nodes of the T1-Pi comparison natively on rpi-1 (aarch64) against the rig's ROS 2 jazzy install (CycloneDDS
# 0.10.5, Fast DDS 2.14.6 with the Pi's fastddsgen 2.3.0) and the rig's zenoh-pico 1.10.2, then copy them to rpi-0. Runs on the PC.
# Sources: fw/ here (the PC comparison's nodes, copied unchanged except fastdds_node.cpp, ported from the 3.x to the 2.14 API).
# It loads rpi-1's CPUs: take the rig lock (flock /tmp/rig.lock) or run it between measurements.
set -eu
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
KEY=${PI_KEY:-$HOME/.ssh/tickle_ci_ed25519}
SSHO="-i $KEY -o StrictHostKeyChecking=no -o BatchMode=yes"
CLI=${CLI_SSH:-ci@10.1.1.214}
SRV=${SRV_SSH:-ci@10.1.1.213}
# shellcheck disable=SC2086
ssh $SSHO "$CLI" 'rm -rf t1pi_fwsrc && mkdir -p t1pi_fwsrc' </dev/null
# shellcheck disable=SC2086
tar -C "$S/fw" -c . | ssh $SSHO "$CLI" 'tar -x -C t1pi_fwsrc'
# shellcheck disable=SC2086
ssh $SSHO "$CLI" 'bash -s' <<'REMOTE'
set -eu
cd "$HOME/t1pi_fwsrc"
BIN=$HOME/t1pi_bin
ROS=/opt/ros/jazzy
LIB=$ROS/lib:$ROS/lib/aarch64-linux-gnu
ZP=$HOME/zenohpico_install_1.10.2
export LD_LIBRARY_PATH=$LIB
mkdir -p gen_cyclone gen_fastdds
# CycloneDDS
(cd gen_cyclone && cp ../Blob.idl . && "$ROS/bin/idlc" -o . Blob.idl)
cc -O2 -I. -Igen_cyclone -I"$ROS/include/CycloneDDS" -o "$BIN/cyclonedds_node" cyclonedds_node.c gen_cyclone/Blob.c \
    -L"$ROS/lib/aarch64-linux-gnu" -L"$ROS/lib" -lddsc -Wl,--disable-new-dtags -Wl,-rpath,"$LIB"
echo "built cyclonedds_node"
# Fast DDS 2.14: generated binding patched as examples/perf_hil/fastdds/build.sh does (two API renames, the GEN_API_VER guard)
(cd gen_fastdds && cp ../Blob.idl . && fastddsgen -replace -d . Blob.idl >gen.log 2>&1)
sed -i -e 's/eprosima::fastcdr::Cdr::DDS_CDR/eprosima::fastcdr::DDS_CDR/g' -e 's/\.getSerializedDataLength()/.get_serialized_data_length()/g' gen_fastdds/*PubSubTypes.cxx
sed -i '/#if !defined(GEN_API_VER)/,/#endif  \/\/ GEN_API_VER/d' gen_fastdds/*PubSubTypes.h
FI=$(find "$ROS"/include -maxdepth 1 -iname fastrtps | head -1)
FC=$(find "$ROS"/include -maxdepth 1 -iname fastcdr | head -1)
g++ -O2 -std=c++17 -I. -Igen_fastdds -I"$FI" -I"$FC" -o "$BIN/fastdds_node" fastdds_node.cpp gen_fastdds/Blob.cxx gen_fastdds/BlobPubSubTypes.cxx \
    -L"$ROS/lib" -L"$ROS/lib/aarch64-linux-gnu" -Wl,--disable-new-dtags -Wl,-rpath,"$LIB" -lfastrtps -lfastcdr -lpthread
echo "built fastdds_node"
# zenoh-pico 1.10.2 (the rig's install; commit recorded)
git -C "$HOME/zenohpico_src_1.10.2" rev-parse HEAD
cc -O2 -DZENOH_LINUX=1 -I. -I"$ZP/include" -o "$BIN/zenohpico_node" zenohpico_node.c -L"$ZP/lib" -lzenohpico -lpthread -lm -Wl,-rpath,"$ZP/lib"
echo "built zenohpico_node"
REMOTE
# shellcheck disable=SC2086
ssh $SSHO "$CLI" 'tar -C t1pi_bin -c cyclonedds_node fastdds_node zenohpico_node' | ssh $SSHO "$SRV" 'tar -x -C t1pi_bin'
# shellcheck disable=SC2086
tar -C "$S" -c t1pi_topo.sh t1pi_clock.py | ssh $SSHO "$CLI" 'tar -x -C t1pi_scripts'
# shellcheck disable=SC2086
tar -C "$S" -c t1pi_topo.sh t1pi_clock.py | ssh $SSHO "$SRV" 'tar -x -C t1pi_scripts'
echo "T1PI_FWBUILD_OK"
