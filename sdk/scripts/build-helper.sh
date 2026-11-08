#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# tvf-web against the SDK-built WebKit (installed into /work/sysroot too).
# In the build container: scripts/run.sh sh scripts/build-helper.sh
set -e
SDK=/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
TP=$SDK/bin/arm-webos-linux-gnueabi
SYSROOT=/work/sysroot
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig
PKGS="wpe-webkit-2.0 wpe-platform-2.0 wpe-platform-headless-2.0 libsoup-3.0 zlib"
mkdir -p /work/build/helper
H=/work/helper-src
$TP-gcc --sysroot=$SYSROOT -O2 -g0 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
    -o /work/build/helper/tvf-web $H/tvf-web.c $H/cJSON.c $(pkg-config --cflags --libs $PKGS) -ldl \
    -Wl,--export-dynamic-symbol=execve -Wl,--export-dynamic-symbol=posix_spawn \
    -Wl,--export-dynamic-symbol=posix_spawnp -Wl,--disable-new-dtags '-Wl,-rpath,$ORIGIN/../lib'
$TP-strip /work/build/helper/tvf-web
ls -l /work/build/helper/tvf-web
