#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-builds WPE WebKit with the webOS SDK (inside the build container:
# scripts/run.sh sh scripts/build-webkit.sh [configure|build|install|all]).
# Source: the pinned tarball, unpacked into build/src and patched with
# patches/webkit/*.patch (in order). Resumable: ninja continues where it stopped.
set -e
SYSROOT=/work/sysroot
J=${JOBS:-3}
V=$(awk '$1 == "wpewebkit" { print $2 }' /work/sources.lock)
S=/work/build/src/wpewebkit-$V
# MEDIA=1: second configuration with media (video, MSE, GStreamer, TVF quirk)
# in its own build and stage directory; the configuration without media stays
if [ "${MEDIA:-0}" = 1 ]; then
    B=/work/build/webkit-media
    STAGE=/work/stage-media
    # EME through WebKit's Thunder CDM against TVF's OpenCDM client
    # (scripts/build-ocdm.sh, helper/tvf-ocdm.c): TVF hosts the TV's CDMs
    OCDM=/work/stage-ocdm/usr
    # (the programs linking libWPEWebKit must find its libocdm.so dependency: rpath-link)
    MEDIA_OPTS="-DENABLE_VIDEO=ON -DUSE_GSTREAMER=ON -DENABLE_MEDIA_SOURCE=ON -DENABLE_ENCRYPTED_MEDIA=ON -DUSE_GSTREAMER_GL=OFF -DUSE_GSTREAMER_MPEGTS=OFF -DENABLE_THUNDER=ON -DTHUNDER_INCLUDE_DIR=$OCDM/include/opencdm -DTHUNDER_LIBRARY=$OCDM/lib/libocdm.so -DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath-link,$OCDM/lib"
else
    B=/work/build/webkit
    STAGE=/work/stage
    MEDIA_OPTS=
fi
SDK=/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
# WebKit's code generators preprocess with $CC (else /usr/bin/gcc as C++
# preprocessor) - use the SDK compiler, the container has no C++ compiler
export CC=$SDK/bin/arm-webos-linux-gnueabi-gcc CXX=$SDK/bin/arm-webos-linux-gnueabi-g++
# glibc 2.12 / Linux 3.10 headers of the SDK: missing constants (compat/)
COMPAT="-include /work/compat/glibc-compat.h"
LFS="-pipe -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig

patch_hash() { cat /work/patches/webkit/*.patch | sha256sum | cut -d' ' -f1; }
patched_files() { grep -h '^+++ b/' /work/patches/webkit/*.patch | sed 's|^+++ b/||' | sort -u; }

unpack() {
    if [ -d "$S" ]; then
        sync_patches
        return
    fi
    mkdir -p /work/build/src
    tar -xf /work/src/wpewebkit-$V.tar.xz -C /work/build/src
    for p in /work/patches/webkit/*.patch; do
        if [ -f "$p" ]; then
            echo "Patch $(basename "$p")"
            patch -d "$S" -p1 --no-backup-if-mismatch < "$p"
        fi
    done
    patch_hash > "$S/.tvf-patches"
    patched_files > "$S/.tvf-patched-files"
}

# The patches changed since the tree was unpacked: the files they touch (now or before) are
# rebuilt from the tarball plus all patches in a scratch tree, and only those that differ are
# copied - ninja then recompiles just what depends on them, not all of WebKit.
sync_patches() {
    h=$(patch_hash)
    [ "$h" = "$(cat "$S/.tvf-patches" 2>/dev/null)" ] && return
    echo "Patches geaendert - betroffene Dateien abgleichen"
    W=/work/build/src/patchsync
    rm -rf "$W" && mkdir -p "$W"
    { patched_files; cat "$S/.tvf-patched-files" 2>/dev/null; } | sort -u > "$W/files"
    sed "s|^|wpewebkit-$V/|" "$W/files" > "$W/members"
    # files the patches create are not in the tarball (tar reports them, that is fine)
    tar -xf /work/src/wpewebkit-$V.tar.xz -C "$W" -T "$W/members" 2> /dev/null || true
    mkdir -p "$W/wpewebkit-$V"
    for p in /work/patches/webkit/*.patch; do
        patch -s -d "$W/wpewebkit-$V" -p1 --no-backup-if-mismatch < "$p"
    done
    while read -r f; do
        if [ -f "$W/wpewebkit-$V/$f" ]; then
            if ! cmp -s "$W/wpewebkit-$V/$f" "$S/$f"; then
                mkdir -p "$(dirname "$S/$f")"
                cp "$W/wpewebkit-$V/$f" "$S/$f"
                echo "  neu: $f"
            fi
        elif [ -f "$S/$f" ]; then
            rm -f "$S/$f"
            echo "  entfernt: $f"
        fi
    done < "$W/files"
    echo "$h" > "$S/.tvf-patches"
    patched_files > "$S/.tvf-patched-files"
    rm -rf "$W"
}

# the options of the last configure run: when they change (e.g. a new
# MEDIA_OPTS), build reconfigures the existing tree instead of keeping the old
# configuration
opts_hash() { printf '%s\n' "$MEDIA_OPTS" "$LFS" "$COMPAT" "$(sed -n '/^configure() {/,/^}/p' "$0")" | sha256sum | cut -d' ' -f1; }

# MEDIA=1: TVF's OpenCDM client first (WebKit's configure checks its headers and links it)
ocdm() {
    [ "${MEDIA:-0}" = 1 ] || return 0
    L=/work/stage-ocdm/usr/lib/libocdm.so
    if [ ! -f "$L" ] || [ -n "$(find /work/helper-src/tvf-ocdm.c /work/helper-src/ocdm -newer "$L" 2>/dev/null)" ]; then
        sh /work/scripts/build-ocdm.sh
    fi
}

configure() {
    unpack
    ocdm
    cmake -S "$S" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE=/work/cross/toolchain.cmake \
        -DPORT=WPE -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DLIB_INSTALL_DIR=/usr/lib \
        -DCMAKE_SKIP_RPATH=ON -DCMAKE_C_FLAGS="$LFS $COMPAT" -DCMAKE_CXX_FLAGS="$LFS $COMPAT" \
        -DENABLE_WPE_PLATFORM=ON -DENABLE_WPE_PLATFORM_HEADLESS=ON -DENABLE_WPE_PLATFORM_DRM=OFF \
        -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF \
        -DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
        -DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_MINIBROWSER=OFF -DENABLE_API_TESTS=OFF \
        -DENABLE_LAYOUT_TESTS=OFF -DENABLE_WEBDRIVER=OFF -DENABLE_WEBDRIVER_BIDI=OFF -DENABLE_COG=OFF \
        -DUSE_ATK=OFF -DUSE_FLITE=OFF -DUSE_SPIEL=OFF -DENABLE_SPEECH_SYNTHESIS=OFF \
        -DUSE_GBM=OFF -DUSE_LIBDRM=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_LIBHYPHEN=OFF \
        -DUSE_SYSPROF_CAPTURE=OFF -DUSE_SYSTEM_SYSPROF_CAPTURE=OFF -DUSE_SYSTEM_UNIFDEF=ON \
        -DUSE_AVIF=OFF -DUSE_LCMS=OFF -DUSE_JPEGXL=OFF -DUSE_WOFF2=ON \
        -DENABLE_XSLT=OFF -DENABLE_SPELLCHECK=OFF -DENABLE_PDFJS=OFF \
        -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DENABLE_MEDIA_STREAM=OFF -DENABLE_MEDIA_RECORDER=OFF \
        -DENABLE_WEB_CODECS=OFF -DENABLE_ENCRYPTED_MEDIA=OFF -DENABLE_MEDIA_SESSION=OFF -DENABLE_WEB_RTC=OFF \
        -DENABLE_WEBGL=OFF -DENABLE_GPU_PROCESS=OFF -DENABLE_GAMEPAD=OFF -DENABLE_WEBXR=OFF \
        -DUSE_VULKAN=OFF -DENABLE_THUNDER=OFF -DUSE_GSTREAMER=OFF \
        -DENABLE_JIT=ON -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF -DENABLE_WEBASSEMBLY=OFF -DENABLE_C_LOOP=OFF \
        $MEDIA_OPTS "$@"
    opts_hash > "$B/.tvf-opts"
}

build() {
    unpack
    ocdm
    if [ ! -f "$B/build.ninja" ] || [ "$(opts_hash)" != "$(cat "$B/.tvf-opts" 2>/dev/null)" ]; then
        echo "Konfiguration neu (Optionen geaendert)"
        configure
    fi
    t0=$(date +%s)
    # KEEP_GOING=1: do not stop at the first failing file (collects all errors)
    ninja -C "$B" -j "$J" ${KEEP_GOING:+-k 0}
    echo "ninja fertig nach $(( $(date +%s) - t0 )) s"
}

install_stage() {
    rm -rf "$STAGE"
    DESTDIR=$STAGE ninja -C "$B" install
    # also into the sysroot: tvf-web is built against it (same API either way;
    # the media build stays out so the sysroot matches the default bundle)
    [ "${MEDIA:-0}" = 1 ] || DESTDIR=$SYSROOT ninja -C "$B" install > /dev/null
}

case "${1:-all}" in
configure) shift; configure "$@" ;;
build) build ;;
install) install_stage ;;
all) build && install_stage ;;
esac
