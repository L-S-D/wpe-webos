#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Offline check of patch 0008 (video hole) under qemu, GPU path via Debian armel Mesa (llvmpipe):
#   docker run --rm -v $PWD/out/tvf-web-sdk-qemu:/b:ro -v $PWD/tests/hole:/t:ro -v /tmp/hole:/o \
#       tvf-web-qemu sh /t/run.sh
# Pages: A hole from the start, B attribute set after 1.5 s, C attribute removed after 1.5 s,
# D control (own layer, no attribute). Background --bg ff00ff = magenta where the frame is transparent.
# Expected: A and B 706400 of 921600 pixels not transparent (hole 640x360 minus the overlay corner
# 190x80 above it), C and D 921600; (300,200) and (300,450) magenta in A/B, black in C/D,
# (700,450) yellow everywhere.
export EGL_PLATFORM=surfaceless TVF_WEB_EXEC_PREFIX="/usr/bin/qemu-arm -cpu cortex-a15"
for v in ${VARIANTS:-A B C D}; do
  URL="data:text/html;base64,$(base64 -w0 /t/page$v.html)"
  env TVF_WEB_EGL_DEFAULT=1 qemu-arm -cpu cortex-a15 -E LD_LIBRARY_PATH=/b/lib /b/bin/tvf-web --url "$URL" --png /o/out$v.png \
      --bg ff00ff --data-dir /tmp/d-$v --settle ${SETTLE:-25000} --timeout 600000 > /o/out$v.log 2>&1
  echo "== $v: rc=$? $(grep -m1 '^PNG' /o/out$v.log)"
done
