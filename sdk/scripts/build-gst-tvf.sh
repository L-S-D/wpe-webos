#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the TVF GStreamer sinks (helper/gsttvf.c) into stage-gst, inside the
# build container: scripts/run.sh sh scripts/build-gst-tvf.sh
set -e
SDK=/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
SYSROOT=/work/sysroot
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig
O=/work/stage-gst/usr/lib/gstreamer-1.0
mkdir -p $O
# shellcheck disable=SC2046
$SDK/bin/arm-webos-linux-gnueabi-gcc --sysroot=$SYSROOT -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
    -o $O/libgsttvf.so /work/helper-src/gsttvf.c $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0)
echo "gebaut: $O/libgsttvf.so"
