#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
URL='data:text/html;charset=utf-8,<body style="margin:0;background:%232050a0"><div style="position:absolute;left:100px;top:300px;width:400px;height:200px;background:%23e03030"></div></body>'
export TVF_WEB_DEBUG=1 TVF_WEB_EXEC_PREFIX="/usr/bin/qemu-arm -cpu cortex-a15"
qemu-arm -cpu cortex-a15 /d/lib/ld-linux-armhf.so.3 --library-path /d/lib /d/bin/tvf-web --url "$URL" --png /o/deb-skia.png \
    --data-dir /tmp/dd --settle 45000 --timeout 600000 > /o/deb-skia.log 2>&1
