#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# In the qemu container (started by tests/mse/run.sh): mse.html in the media bundle's tvf-web,
# console and sink log to /o/mse.log, picture to /o/mse.png (--bg ff00ff: magenta = transparent,
# the video hole). /b = bundle copy for qemu, /o = content directory served on 127.0.0.1:8791.
export EGL_PLATFORM=surfaceless TVF_WEB_EXEC_PREFIX="/usr/bin/qemu-arm -cpu cortex-a15"
export WEBKIT_GST_QUIRKS=tvf WEBKIT_GST_HOLE_PUNCH_QUIRK=tvf GST_REGISTRY_FORK=no GST_DEBUG=${GST_DEBUG:-2}
env TVF_WEB_EGL_DEFAULT=1 qemu-arm -cpu cortex-a15 -E LD_LIBRARY_PATH=/b/lib /b/bin/tvf-web --media \
    --url "${URL:-http://127.0.0.1:8791/mse.html}" --png /o/mse.png --bg ff00ff --data-dir /tmp/d-mse \
    --settle "${SETTLE:-90000}" --timeout 900000 > /o/mse.log 2>&1
echo "rc=$? $(grep -m1 '^PNG' /o/mse.log)"
