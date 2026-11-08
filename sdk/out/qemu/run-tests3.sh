#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
URL='data:text/html;charset=utf-8,<body style="margin:0;background:%232050a0"><div style="position:absolute;left:100px;top:300px;width:400px;height:200px;background:%23e03030"></div></body>'
export EGL_PLATFORM=surfaceless TVF_WEB_GL_DEBUG=1 TVF_WEB_EXEC_PREFIX="/usr/bin/qemu-arm -cpu cortex-a15"
qemu-arm -cpu cortex-a15 -E LD_LIBRARY_PATH=/b/lib /b/bin/tvf-web --url "$URL" --png /o/q-ctx.png \
    --data-dir /tmp/d-ctx --settle 30000 --timeout 600000 > /o/q-ctx.log 2>&1
