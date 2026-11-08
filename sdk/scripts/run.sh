#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Run a command in the build container: SDK read-only at its own path,
# sdk/ as /work, low CPU and I/O priority (TVH streams from this machine).
SDK=/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
# WEBOS_SDK_HOST: the SDK somewhere else on the host (e.g. WSL, no sudo);
# in the container it is always at $SDK (the cross files expect that path)
SDK_HOST=${WEBOS_SDK_HOST:-$SDK}
D=$(cd "$(dirname "$0")/.." && pwd)
# not root (WSL user): run as that user, so the build tree stays theirs
U=
[ "$(id -u)" = 0 ] || U="--user $(id -u):$(id -g) -e HOME=/tmp"
# shellcheck disable=SC2086
exec docker run --rm $U -v "$SDK_HOST:$SDK:ro" -v "$D:/work" -v "$D/../helper:/work/helper-src:ro" -w /work -e JOBS="${JOBS:-3}" -e KEEP_GOING="${KEEP_GOING:-}" -e MEDIA="${MEDIA:-}" tvf-web-sdkbuild \
    nice -n 19 ionice -c3 "$@"
