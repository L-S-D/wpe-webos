#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-builds WebKit's missing dependencies into the writable sysroot copy
# (/work/sysroot, prefix /usr). Runs inside the build container
# (scripts/run.sh scripts/build-deps.sh [package ...]). A package that was
# built is skipped (build/<pkg>.done); logs in logs/<pkg>.log.
set -e
SDK=/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
T=arm-webos-linux-gnueabi
SYSROOT=/work/sysroot
J=${JOBS:-3}
# SDK tools by full path only: $SDK/bin also has host tools (cmake, ...) that need old host libraries
TP=$SDK/bin/$T
# only the cross tools on PATH (configure scripts call $host-objdump etc. by name)
mkdir -p /work/build/toolbin
for t in $SDK/bin/$T-*; do ln -sf "$t" /work/build/toolbin/; done
export PATH=/work/build/toolbin:$PATH
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig
CFL="-O2 -pipe -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"
mkdir -p /work/build /work/logs

if [ ! -d $SYSROOT/usr/include ]; then
    echo "Sysroot-Kopie anlegen"
    cp -a $SDK/$T/sysroot $SYSROOT
fi

ver() { awk -v n="$1" '$1 == n { print $2 }' /work/sources.lock; }

unpack() { # name -> sets S (source dir)
    v=$(ver "$1")
    f=$(ls /work/src/"$1-$v".tar.* | head -1)
    S=/work/build/src/$1-$v
    if [ ! -d "$S" ]; then
        mkdir -p /work/build/src/tmp-$1 && tar -xf "$f" -C /work/build/src/tmp-$1
        mv /work/build/src/tmp-$1/* "$S" && rmdir /work/build/src/tmp-$1
        for p in /work/patches/deps/$1-*.patch; do
            if [ -f "$p" ]; then patch -d "$S" -p1 < "$p"; fi
        done
    fi
}

meson_pkg() { # name options...
    n=$1; shift
    unpack "$n"
    B=/work/build/$n
    rm -rf "$B"
    meson setup "$B" "$S" --cross-file /work/cross/meson-cross.ini --prefix=/usr --libdir=lib \
        --buildtype=release -Ddefault_library=shared -Dstrip=false "$@"
    ninja -C "$B" -j "$J"
    DESTDIR=$SYSROOT ninja -C "$B" install
}

cmake_pkg() { # name options...
    n=$1; shift
    unpack "$n"
    B=/work/build/$n
    rm -rf "$B"
    cmake -S "$S" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE=/work/cross/toolchain.cmake \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=ON "$@"
    ninja -C "$B" -j "$J"
    DESTDIR=$SYSROOT ninja -C "$B" install
}

auto_pkg() { # name host options...
    n=$1; host=$2; shift 2
    unpack "$n"
    B=/work/build/$n
    rm -rf "$B" && mkdir -p "$B" && cd "$B"
    CC="$TP-gcc --sysroot=$SYSROOT" CXX="$TP-g++ --sysroot=$SYSROOT" AR=$TP-ar RANLIB=$TP-ranlib STRIP=$TP-strip \
        OBJDUMP=$TP-objdump NM=$TP-nm \
        CFLAGS="$CFL" CXXFLAGS="$CFL" "$S/configure" --host="$host" --build=x86_64-linux-gnu \
        --prefix=/usr --libdir=/usr/lib --disable-static --enable-shared "$@"
    make -j "$J"
    make DESTDIR=$SYSROOT install
    cd /work
}

build() {
    n=$1
    [ -f /work/build/$n.done ] && { echo "== $n schon gebaut"; return; }
    echo "== $n ($(ver "$n"))"
    t0=$(date +%s)
    set +e
    sh /work/scripts/build-deps.sh --one "$n" > /work/logs/$n.log 2>&1
    rc=$?
    set -e
    if [ $rc -ne 0 ]; then
        echo "FEHLER bei $n, siehe logs/$n.log"; tail -25 /work/logs/$n.log; exit 1
    fi
    find $SYSROOT/usr/lib -maxdepth 1 -name '*.la' -newer /work/sources.lock -delete
    touch /work/build/$n.done
    echo "   fertig in $(( $(date +%s) - t0 )) s"
}

build_brotli() { cmake_pkg brotli -DBROTLI_DISABLE_TESTS=ON; }
build_nghttp2() { cmake_pkg nghttp2 -DENABLE_LIB_ONLY=ON -DENABLE_DOC=OFF -DBUILD_STATIC_LIBS=OFF; }
build_libpsl() { meson_pkg libpsl -Druntime=libicu -Dbuiltin=true -Dtests=false -Ddocs=false; }
build_libsoup() {
    meson_pkg libsoup -Dgssapi=disabled -Dntlm=disabled -Dbrotli=enabled -Dtls_check=false -Dintrospection=disabled \
        -Dvapi=disabled -Ddocs=disabled -Dtests=false -Dautobahn=disabled -Dsysprof=disabled -Dpkcs11_tests=disabled
}
build_glib_networking() {
    meson_pkg glib-networking -Dgnutls=disabled -Dopenssl=enabled -Dlibproxy=disabled -Dgnome_proxy=disabled \
        -Dinstalled_tests=false
    # giomoduledir from pkg-config already carries the sysroot, DESTDIR adds it again
    mkdir -p $SYSROOT/usr/lib/gio/modules
    mv $SYSROOT$SYSROOT/usr/lib/gio/modules/* $SYSROOT/usr/lib/gio/modules/ && rm -rf $SYSROOT/work
}
build_harfbuzz() {
    meson_pkg harfbuzz -Dicu=enabled -Dfreetype=enabled -Dglib=enabled -Dgobject=disabled -Dcairo=disabled \
        -Dchafa=disabled -Dintrospection=disabled -Dtests=disabled -Ddocs=disabled -Dutilities=disabled \
        -Dbenchmark=disabled
}
build_libwebp() {
    cmake_pkg libwebp -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
        -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
        -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=ON
}
build_woff2() { cmake_pkg woff2 -DCANONICAL_PREFIXES=ON; }
build_libepoxy() { meson_pkg libepoxy -Degl=yes -Dglx=no -Dx11=false -Dtests=false -Ddocs=false; }
build_libgpg_error() {
    # the lock object header is generated from the compiler output (needs OBJDUMP);
    # a mismatch aborts at run time with "gpgrt fatal: sizeof lock obj"
    auto_pkg libgpg-error $T --disable-doc --disable-tests --disable-languages \
        --disable-nls --disable-install-gpg-error-config
    grep -q "lock-obj-pub.native.h using $TP-objdump" config.log 2>/dev/null || grep -q "using .*objdump" /work/build/libgpg-error/config.log
}
build_libgcrypt() {
    # configure looks for gpgrt-config only below --prefix (= host /usr) and in PATH
    export GPGRT_CONFIG=$SYSROOT/usr/bin/gpgrt-config
    auto_pkg libgcrypt $T --disable-doc --disable-padlock-support --disable-jent-support
}
build_libtasn1() { auto_pkg libtasn1 $T --disable-doc --disable-valgrind-tests; }

# GStreamer plugins for WebKit's media code (MEDIA=1 build). The SDK has
# GStreamer 1.24.11 with the base libraries but only a few plugins; these
# are built against it into /work/stage-gst, NOT into the sysroot: WebKit
# compiles against the SDK's headers, the plugins use the SDK's libraries
# at run time (same version). Not part of the default list:
#   scripts/run.sh sh scripts/build-deps.sh gst_plugins_base gst_plugins_good gst_plugins_bad
GST_COMMON="-Dauto_features=disabled -Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dnls=disabled -Dorc=disabled"
gst_pkg() { # name options...
    n=$1; shift
    unpack "$n"
    B=/work/build/$n
    rm -rf "$B"
    meson setup "$B" "$S" --cross-file /work/cross/meson-cross.ini --prefix=/usr --libdir=lib \
        --buildtype=release -Ddefault_library=shared -Dstrip=false $GST_COMMON "$@"
    ninja -C "$B" -j "$J"
    DESTDIR=/work/stage-gst ninja -C "$B" install
}
# appsrc/appsink (WebKit's MSE append pipeline)
build_gst_plugins_base() { gst_pkg gst-plugins-base -Dapp=enabled -Dtools=disabled -Dintrospection=disabled; }
# qtdemux (MP4/fMP4), aacparse/ac3parse/mpegaudioparse, matroskademux (WebM), id3demux (HLS with
# packed audio)
build_gst_plugins_good() {
    gst_pkg gst-plugins-good -Disomp4=enabled -Daudioparsers=enabled -Dmatroska=enabled -Did3demux=enabled
}
# h264parse/h265parse (parsebin in WebKit's playback pipeline), tsdemux (HLS with TS segments),
# dashdemux/hlsdemux for DASH and HLS straight in <video> (HbbTV pages set an MPD as src; WebKit
# enables these with WEBKIT_GST_ENABLE_DASH/HLS_SUPPORT and keeps the adaptivedemux2 ones at rank
# NONE - these fetch segments through WebKit's own source, i.e. WebKit's network process with its
# proxy and cookies; AES-128 HLS with OpenSSL), webvttenc/srtenc
# (subenc: WebKit's text combiner and MSE text tracks convert subtitles to WebVTT;
# closedcaption/CEA-608 would need pangocairo, which the SDK lacks)
build_gst_plugins_bad() {
    gst_pkg gst-plugins-bad -Dvideoparsers=enabled -Dsubenc=enabled -Dmpegtsdemux=enabled -Ddash=enabled \
        -Dhls=enabled -Dhls-crypto=openssl -Dtools=disabled -Dintrospection=disabled
}

# one package in its own shell, so that set -e stops at the first failing step
if [ "$1" = "--one" ]; then
    "build_$2"
    exit 0
fi

[ $# -gt 0 ] || set -- brotli nghttp2 libpsl libsoup glib_networking harfbuzz libwebp woff2 libepoxy \
    libgpg_error libgcrypt libtasn1
for p in "$@"; do build "$p"; done
echo "Abhaengigkeiten fertig"
