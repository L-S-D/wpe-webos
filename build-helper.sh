#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build tvf-web and libtvfca.so in the build image: armhf (TV) and amd64
# (host tests). Output: build/armhf, build/amd64.
set -e
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/work -w /work tvf-web-build sh -ec '
PKGS="wpe-webkit-2.0 wpe-platform-2.0 wpe-platform-headless-2.0 libsoup-3.0 zlib"
EXP="-Wl,--export-dynamic-symbol=execve -Wl,--export-dynamic-symbol=posix_spawn -Wl,--export-dynamic-symbol=posix_spawnp"
mkdir -p build/armhf build/amd64
A=/usr/lib/arm-linux-gnueabihf
PKG_CONFIG_LIBDIR=$A/pkgconfig:/usr/share/pkgconfig pkg-config --cflags --libs $PKGS > build/armhf/flags
arm-linux-gnueabihf-gcc -O2 -g0 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -o build/armhf/tvf-web \
    helper/tvf-web.c helper/cJSON.c $(cat build/armhf/flags) -ldl $EXP -Wl,-rpath-link=$A
arm-linux-gnueabihf-gcc -O2 -g0 -Wall -shared -fPIC -o build/armhf/libtvfca.so helper/tvfca.c $A/libgnutls.so.30
arm-linux-gnueabihf-strip build/armhf/tvf-web build/armhf/libtvfca.so
X=/usr/lib/x86_64-linux-gnu
gcc -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -o build/amd64/tvf-web helper/tvf-web.c helper/cJSON.c \
    $(pkg-config --cflags --libs $PKGS) -ldl $EXP
gcc -O2 -Wall -shared -fPIC -o build/amd64/libtvfca.so helper/tvfca.c $X/libgnutls.so.30
ls -l build/armhf build/amd64
'
