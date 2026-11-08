#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# after the Skia raster pipeline ABI fix: CPU painting and Skia's own readback must be correct too
export EGL_PLATFORM=surfaceless TVF_WEB_GL_DEBUG=1 TVF_WEB_EXEC_PREFIX="/usr/bin/qemu-arm -cpu cortex-a15"
URL="data:text/html;base64,$(base64 -w0 /o/rich.html)"
run() {
    name=$1; shift
    env "$@" qemu-arm -cpu cortex-a15 -E LD_LIBRARY_PATH=/b/lib /b/bin/tvf-web --url "$URL" --png /o/$name.png \
        --data-dir /tmp/d-$name --settle 40000 --timeout 900000 > /o/$name.log 2>&1
    echo "== $name: rc=$? $(grep -m1 '^PNG' /o/$name.log)"
}
run abi-gpu TVF_WEB_EGL_DEFAULT=1
run abi-skiaread TVF_WEB_EGL_DEFAULT=1 TVF_WEB_SKIA_READBACK=1
run abi-cpu TVF_WEB_EGL_DEFAULT=1 TVF_WEB_CPU=1
