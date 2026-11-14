#!/usr/bin/env bash
# Build the T1 wmediumd: libconfig (static) + wmediumd 0.3.1 with wmediumd-0.3.1-t1.patch, without root (links the system libnl .so.200).
#   WM_ROOT=$HOME/tmp_build/wm ./t1_build_wmediumd.sh
# Needs: git, cmake, make, gcc, libnl-3-dev + libnl-genl-3-dev (headers in /usr/include/libnl3); x86_64 or aarch64 Linux
# (the libnl directory comes from dpkg-architecture, or set LIBDIR).
# Result: $WM_ROOT/wmediumd/wmediumd/wmediumd, the default of WM in t1_topo.sh.
set -eu
W=${WM_ROOT:-$HOME/tmp_build/wm}
P=$W/prefix
L=${LIBDIR:-/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)}
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$W" "$P/lib/pkgconfig" "$P/include"
[ -d "$W/libconfig" ] || git clone -q --branch v1.7.3 https://github.com/hyperrealm/libconfig "$W/libconfig"
if [ ! -d "$W/wmediumd" ]; then
    git clone -q https://github.com/bcopeland/wmediumd "$W/wmediumd"
    git -C "$W/wmediumd" checkout -q 717e5d7fcc23eecbc8e32bd897a8fd4b1e3ba640 # the 0.3.1 base the patch was made against
    git -C "$W/wmediumd" apply "$S/wmediumd-0.3.1-t1.patch"
fi
ln -sf "$L/libnl-genl-3.so.200" "$P/lib/libnl-genl-3.so"
ln -sf "$L/libnl-3.so.200" "$P/lib/libnl-3.so"
cmake -S "$W/libconfig" -B "$W/libconfig/build" -DCMAKE_INSTALL_PREFIX="$P" -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >"$W/libconfig.log" 2>&1
cmake --build "$W/libconfig/build" -j8 >>"$W/libconfig.log" 2>&1
cmake --install "$W/libconfig/build" >>"$W/libconfig.log" 2>&1
echo LIBCONFIG_OK
export PKG_CONFIG_PATH=$P/lib/pkgconfig:$P/lib64/pkgconfig
# CFLAGS/LDFLAGS go in through the environment: a make command-line variable would override the Makefile's own
# `CFLAGS +=` lines and lose -Iinc and -DVERSION_STR (the build fails or reports no version).
CFLAGS="-I/usr/include/libnl3 -I$P/include" LDFLAGS="-L$P/lib -lm" make -C "$W/wmediumd/wmediumd" >"$W/wmediumd.log" 2>&1
echo BUILD_OK "$W/wmediumd/wmediumd/wmediumd"
