#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds libocdm.so (helper/tvf-ocdm.c: OpenCDM client of TVF, see there) and
# installs it with its headers into stage-ocdm, inside the build container:
#   scripts/run.sh sh scripts/build-ocdm.sh
# The media build of WebKit (MEDIA=1) compiles its Thunder CDM against these
# headers and links the library (ENABLE_THUNDER); the library itself can be
# rebuilt on its own as long as the headers stay the same.
set -e
SDK=/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
SYSROOT=/work/sysroot
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig
O=/work/stage-ocdm/usr
mkdir -p $O/lib $O/include/opencdm
cp /work/helper-src/ocdm/open_cdm.h /work/helper-src/ocdm/open_cdm_adapter.h $O/include/opencdm/
# shellcheck disable=SC2046
$SDK/bin/arm-webos-linux-gnueabi-gcc --sysroot=$SYSROOT -shared -fPIC -O2 -Wall -Wextra \
    -fvisibility=hidden -Wl,-soname,libocdm.so -I/work/helper-src \
    -o $O/lib/libocdm.so /work/helper-src/tvf-ocdm.c $(pkg-config --cflags --libs gstreamer-1.0) -lpthread
echo "gebaut: $O/lib/libocdm.so"
