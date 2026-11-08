#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# offline WebKit GL tests under qemu-arm with Debian armel Mesa (surfaceless)
URL='data:text/html;charset=utf-8,<body style="margin:0;background:%232050a0;color:%23fff;font:48px sans-serif"><div style="position:absolute;left:100px;top:300px;width:400px;height:200px;background:%23e03030">rot</div><p style="position:absolute;left:600px;top:300px">Hallo TVF</p></body>'
export EGL_PLATFORM=surfaceless TVF_WEB_GL_DEBUG=1 TVF_WEB_EXEC_PREFIX="/usr/bin/qemu-arm -cpu cortex-a15"
run() {
    name=$1; shift
    env "$@" qemu-arm -cpu cortex-a15 -E LD_LIBRARY_PATH=/b/lib /b/bin/tvf-web --url "$URL" --png /o/$name.png \
        --data-dir /tmp/d-$name --settle 6000 --timeout 600000 > /o/$name.log 2>&1
    echo "== $name: $(grep -c . /o/$name.log) Zeilen"
}
run surfaceless
run default TVF_WEB_EGL_DEFAULT=1
run default-pattern TVF_WEB_EGL_DEFAULT=1 TVF_WEB_TESTPATTERN=1
