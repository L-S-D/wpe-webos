#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Skia composition with the glReadPixels(GL_BGRA) readback fix: text, JPEG, PNG, gradient layer, canvas
export EGL_PLATFORM=surfaceless TVF_WEB_GL_DEBUG=1 TVF_WEB_EXEC_PREFIX="/usr/bin/qemu-arm -cpu cortex-a15"
URL="data:text/html;base64,$(base64 -w0 /o/rich.html)"
run() {
    name=$1; shift
    env "$@" qemu-arm -cpu cortex-a15 -E LD_LIBRARY_PATH=/b/lib /b/bin/tvf-web --url "$URL" --png /o/$name.png \
        --data-dir /tmp/d-$name --settle 40000 --timeout 900000 > /o/$name.log 2>&1
    echo "== $name: rc=$? $(grep -c . /o/$name.log) Zeilen"
}
run fix-default TVF_WEB_EGL_DEFAULT=1
