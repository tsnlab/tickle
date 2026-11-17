#!/usr/bin/env bash
# Builds, from source, the DDS versions the rig and the Pi T1 run use, for the PC T1 framework comparison (x86_64; the rig's
# aarch64 ros-jazzy packages differ in architecture only): Fast DDS 2.14.6 (ROS 2 jazzy's fastrtps line), Fast CDR 2.2.7,
# foonathan_memory 0.7-3, Fast-DDS-Gen 2.3.0 (the Pis' fastddsgen is 2.3.0) and CycloneDDS 0.10.5 (jazzy). Pinned tags; the commit each
# resolved to is checked against the pin and the build refuses on a mismatch. Everything goes under $T1FW_DEPS/prefix (default
# $HOME/t1fw_v214/prefix); nothing system-wide. Take `flock /tmp/gates.lock` around it: builds must not overlap a measurement.
# Cyclone is built without iceoryx/SHM and without security (distinct namespaces cannot use SHM, and the comparison forces UDP).
# Simulation instrument; no library code is changed.
set -euo pipefail
ROOT=${T1FW_DEPS:-$HOME/t1fw_v214}
PREFIX=$ROOT/prefix
SRC=$ROOT/src
J=${JOBS:-$(nproc)}
mkdir -p "$PREFIX" "$SRC"
clone() { # name url tag commit
    if [ ! -d "$SRC/$1/.git" ]; then git clone -q -c advice.detachedHead=false --branch "$3" --depth 1 --recurse-submodules --shallow-submodules "$2" "$SRC/$1"; fi
    [ "$(git -C "$SRC/$1" rev-parse HEAD)" = "$4" ] || { echo "REFUSING: $1 $3 resolved to $(git -C "$SRC/$1" rev-parse HEAD), pin is $4"; exit 1; }
    echo "pinned $1 $3 $4"
}
cmk() { # name extra-cmake-args...
    local n=$1
    shift
    cmake -S "$SRC/$n" -B "$SRC/$n/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DBUILD_TESTING=OFF "$@" >"$SRC/$n.cmake.log" 2>&1
    cmake --build "$SRC/$n/build" --parallel "$J" >"$SRC/$n.build.log" 2>&1
    cmake --install "$SRC/$n/build" >>"$SRC/$n.build.log" 2>&1
    echo "installed $n"
}
clone fastcdr https://github.com/eProsima/Fast-CDR.git v2.2.7 4611cea8cf805befc6cac8fadf0e8f75fc92405a
clone foonathan_memory https://github.com/foonathan/memory.git v0.7-3 0f0775770fd1c506fa9c5ad566bd6ba59659db66
clone fastdds https://github.com/eProsima/Fast-DDS.git v2.14.6 f052f239538ebc0df56835fdb57f7628628010bc
clone cyclonedds https://github.com/eclipse-cyclonedds/cyclonedds.git 0.10.5 2cdd114cbd18340c606573b4cc8dc20cc161ec5a
clone fastddsgen https://github.com/eProsima/Fast-DDS-Gen.git v2.3.0 2e7550a3b5fd5ed6e17814ee7590542f7fda523a
cmk fastcdr
cmk foonathan_memory -DFOONATHAN_MEMORY_BUILD_EXAMPLES=OFF -DFOONATHAN_MEMORY_BUILD_TESTS=OFF -DFOONATHAN_MEMORY_BUILD_TOOLS=OFF
cmk fastdds -DTHIRDPARTY=ON -DSECURITY=OFF -DCOMPILE_EXAMPLES=OFF
cmk cyclonedds -DBUILD_IDLC=ON -DENABLE_SECURITY=OFF -DENABLE_SSL=OFF -DENABLE_SHM=OFF -DBUILD_DDSPERF=OFF
# Fast-DDS-Gen 2.3.0: a gradle 7.0 build (the wrapper downloads gradle), which cannot run on a JDK newer than 16. T1FW_JAVA_HOME names a
# JDK 11 (the build used Temurin 11.0.33 unpacked under $ROOT/jdk); the generator itself then runs on any JDK 11+.
(
    cd "$SRC/fastddsgen"
    export JAVA_HOME=${T1FW_JAVA_HOME:?set T1FW_JAVA_HOME to a JDK 11} PATH=$T1FW_JAVA_HOME/bin:$PATH
    ./gradlew assemble install --install_path="$PREFIX" >"$SRC/fastddsgen.build.log" 2>&1
)
mkdir -p "$PREFIX/share/fastddsgen/java"; cp "$SRC/fastddsgen/share/fastddsgen/java/fastddsgen.jar" "$PREFIX/share/fastddsgen/java/"
echo "installed fastddsgen"
echo "DEPS_OK $PREFIX"
