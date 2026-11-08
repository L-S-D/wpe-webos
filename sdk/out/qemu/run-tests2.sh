#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
URL='data:text/html;charset=utf-8,<body style="margin:0;background:%232050a0"><div style="position:absolute;left:100px;top:300px;width:400px;height:200px;background:%23e03030"></div></body>'
export EGL_PLATFORM=surfaceless TVF_WEB_GL_DEBUG=1 TVF_WEB_EGL_DEFAULT=1 TVF_WEB_EXEC_PREFIX="/usr/bin/qemu-arm -cpu cortex-a15"
run() {
    name=$1; shift
    env "$@" qemu-arm -cpu cortex-a15 -E LD_LIBRARY_PATH=/b/lib /b/bin/tvf-web --url "$URL" --png /o/$name.png \
        --data-dir /tmp/d-$name --settle 45000 --timeout 600000 > /o/$name.log 2>&1
}
run q-pattern TVF_WEB_TESTPATTERN=1
run q-texmap WEBKIT_USE_SKIA_FOR_COMPOSITION=0
run q-texmap-cpu WEBKIT_USE_SKIA_FOR_COMPOSITION=0 TVF_WEB_CPU=1
