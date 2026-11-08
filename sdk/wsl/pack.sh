#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build on a faster machine (WSL2 + Docker, e.g. a Windows PC in the LAN): packs what the build
# needs, serves it in the LAN with a random token and receives the bundle back.
#   sh wsl/pack.sh full     first time: work tree with sources and sysroot, webOS SDK, build image
#   sh wsl/pack.sh update   later: the repo's files (incl. uncommitted changes) and stage-gst
# Prints the one command for the build machine (in WSL), then serves until Ctrl-C. Bundle and
# logs arrive in out/wsl/upload. WSL_HOST_IP / WSL_PORT override address and port; the SDK is
# taken from WEBOS_SDK_HOST (default /opt/webos-sdk/...). The TV libraries (fromtv/) and the
# SDK go to the build machine only - never anywhere else.
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
R=$(dirname "$S")
MODE=${1:-update}
IP=${WSL_HOST_IP:-$(hostname -I | cut -d' ' -f1)}
PORT=${WSL_PORT:-8799}
TOKEN=$(python3 -c 'import secrets; print(secrets.token_hex(8))')
SDK=${WEBOS_SDK_HOST:-/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}
O=$S/out/wsl
Z=$(command -v pigz || echo gzip)
mkdir -p "$O/serve" "$O/upload" "$O/top"
sub() { sed -e "s|@SRC@|http://$IP:$PORT/$TOKEN|g" "$S/wsl/$1.in"; }
sub setup-wsl.sh > "$O/serve/setup-wsl.sh"
sub update-wsl.sh > "$O/serve/update-wsl.sh"
sub wsl-build.sh > "$O/top/wsl-build.sh"
sub LIESMICH.md > "$O/top/LIESMICH.md"
cp "$O/top/LIESMICH.md" "$O/serve/"

# work tree: the repo's files (tracked and new ones, not the ignored build output) ...
cd "$R"
git ls-files -co --exclude-standard | while read -r f; do [ -e "$f" ] && echo "$f"; done > "$O/files.txt"
echo sdk/stage-gst >> "$O/files.txt"
if [ "$MODE" = full ]; then
    # ... plus what the build needs from outside the repo: sources (not the unpacked WebKit tree,
    # mkpatch.sh's reference - tar -xf src/wpewebkit-*.tar.xz -C src), sysroot with the built
    # dependencies, TV libraries, patchwork, build markers, tvf-web, data files for bundle-sdk.py
    { ls -d sdk/src/* | grep -v '/wpewebkit-[0-9.]*$'
      echo sdk/sysroot; echo sdk/fromtv; echo sdk/patchwork; echo sdk/build/helper/tvf-web
      ls -d sdk/build/*.done
      echo out/tvf-web-armhf/etc; echo out/tvf-web-armhf/share/fonts; echo out/tvf-web-armhf/share/X11
    } >> "$O/files.txt"
    A=wpe-webos.tar
else
    A=wpe-webos-update.tar
fi
rm -f "$O/serve/wpe-webos-update.tar.gz"
nice -n 19 tar -cf "$O/$A" --transform 's|^|wpe-webos/|S' -T "$O/files.txt"
tar -rf "$O/$A" -C "$O/top" wsl-build.sh LIESMICH.md
nice -n 19 $Z -f "$O/$A" && mv "$O/$A.gz" "$O/serve/"
if [ "$MODE" = full ]; then
    # SDK and build image change rarely: reused when already packed
    [ -f "$O/serve/webos-sdk.tar.gz" ] ||
        nice -n 19 tar -c -C "$(dirname "$SDK")" "$(basename "$SDK")" | nice -n 19 $Z > "$O/serve/webos-sdk.tar.gz"
    [ -f "$O/serve/image-sdkbuild.tar.gz" ] ||
        docker save tvf-web-sdkbuild:latest | nice -n 19 $Z > "$O/serve/image-sdkbuild.tar.gz"
    CMD=setup-wsl.sh
else
    CMD=update-wsl.sh
fi
ls -la "$O/serve"
echo
echo "Auf dem Bau-Rechner in WSL:"
echo "  curl -fsSO http://$IP:$PORT/$TOKEN/$CMD && sh $CMD"
echo "Ergebnis kommt nach $O/upload (Dienst mit Ctrl-C beenden)"
exec python3 -I "$S/wsl/serve.py" "$IP" "$PORT" "$TOKEN" "$O/serve" "$O/upload"
