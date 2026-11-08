#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# OpenCDM client (helper/tvf-ocdm.c) against TVF's media server with a simulated
# CDM and decoder - on the host, no TV, no WebKit:
#   in TVF:  sh tools/webmedia-test/build.sh && build/webmedia-test/webmedia-test 60 > /tmp/wm.log 2>&1 &
#   here:    sh sdk/tests/eme/run.sh
# (TVF_MEDIA_SOCK defaults to the harness' @tvf-media-test.)
set -e
cd "$(dirname "$0")/../../.."
O=sdk/out/eme
mkdir -p $O
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -DTVF_OCDM_NO_GST -Ihelper \
    $(pkg-config --cflags glib-2.0) -o $O/ocdm-test sdk/tests/eme/ocdm-test.c helper/tvf-ocdm.c \
    $(pkg-config --libs glib-2.0) -lpthread
TVF_MEDIA_SOCK=${TVF_MEDIA_SOCK:-@tvf-media-test} $O/ocdm-test
