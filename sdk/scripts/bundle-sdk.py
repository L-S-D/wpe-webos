#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tvf-web bundle from the webOS-SDK build (runs on the host, needs readelf).

bundle-sdk.py OUTDIR [--jsc] [--media]
WebKit (stage/), our dependencies and the SDK's libraries (sysroot/) are bundled;
--media: the build with media (stage-media/: video, MSE, EME, GStreamer) plus
the GStreamer plugins it needs (ours from stage-gst/, the rest from the SDK);
glibc, EGL/GLES (Mali) and Wayland come from the TV. Data files (CA bundle,
fonts, xkb) are taken from the Debian bundle in ../out/tvf-web-armhf.
"""
import glob, os, re, shutil, subprocess, sys, tarfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1]
with_jsc = "--jsc" in sys.argv[2:]  # also the jsc shell (JavaScript tests)
with_media = "--media" in sys.argv[2:]
STAGE = f"{HERE}/stage-media/usr" if with_media else f"{HERE}/stage/usr"
SYSROOT = f"{HERE}/sysroot"
GST = f"{HERE}/stage-gst/usr/lib"
OCDM = f"{HERE}/stage-ocdm/usr/lib"  # TVF's OpenCDM client (EME, scripts/build-ocdm.sh)
DEB = f"{HERE}/../out/tvf-web-armhf"
# GStreamer's base libraries from the SDK (WebKit is linked against them),
# from stage-gst only what the SDK lacks (codecparsers for the video parsers)
SEARCH = [f"{STAGE}/lib", f"{SYSROOT}/usr/lib", f"{SYSROOT}/lib"] + ([GST, OCDM] if with_media else [])
# plugins: appsrc/appsink, qtdemux, parsers, matroska and the TVF sinks are
# built here; queue/typefind/playbin3 etc. come from the SDK
GST_OURS = ["libgstapp.so", "libgstisomp4.so", "libgstaudioparsers.so", "libgstmatroska.so",
            "libgstvideoparsersbad.so", "libgstsubenc.so", "libgstdash.so", "libgsthls.so", "libgstid3demux.so",
            "libgstmpegtsdemux.so", "libgsttvf.so"]
GST_SDK = ["libgstcoreelements.so", "libgsttypefindfunctions.so", "libgstplayback.so"]
# from the TV: glibc, the GPU driver and the Wayland client it uses
FROM_TV = {"libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1", "libdl.so.2", "ld-linux.so.3",
           "libresolv.so.2", "libutil.so.1", "libcrypt.so.1", "libEGL.so.1", "libGLESv2.so.2", "libgbm.so.1",
           "libwayland-client.so.0", "libwayland-server.so.0", "libwayland-egl.so.1"}

shutil.rmtree(out, ignore_errors=True)
for d in ["bin", "lib/gio/modules", "libexec/injected-bundle", "etc/fonts", "etc/ssl", "share/fonts"]:
    os.makedirs(f"{out}/{d}", exist_ok=True)

# the SDK's binutils (no host binutils needed); WEBOS_SDK_HOST as in run.sh
SDKBIN = os.environ.get("WEBOS_SDK_HOST", "/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot") \
    + "/bin/arm-webos-linux-gnueabi-"

def needed(path):
    r = subprocess.run([SDKBIN + "readelf", "-d", path], capture_output=True, text=True, check=True).stdout
    return [l.split("[", 1)[1].rstrip("]") for l in r.splitlines() if "(NEEDED)" in l]

def find(name):
    for d in SEARCH:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None

copied, missing, tv, queue = set(), set(), set(), []

def add_file(src, dst):
    shutil.copy2(os.path.realpath(src), dst)
    queue.append(dst)

def add_lib(name):
    if name in FROM_TV:
        tv.add(name)
        return
    if name in copied:
        return
    p = find(name)
    if not p:
        missing.add(name)
        return
    copied.add(name)
    add_file(p, f"{out}/lib/{name}")

add_file(f"{HERE}/build/helper/tvf-web", f"{out}/bin/tvf-web")
if with_jsc:
    add_file(f"{HERE}/build/webkit/bin/jsc", f"{out}/bin/jsc")
for p in ["WPEWebProcess", "WPENetworkProcess", "WPEGPUProcess"]:
    f = f"{STAGE}/libexec/wpe-webkit-2.0/{p}"
    if os.path.exists(f):
        add_file(f, f"{out}/libexec/{p}")
add_file(f"{STAGE}/lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so",
         f"{out}/libexec/injected-bundle/libWPEInjectedBundle.so")
add_file(f"{SYSROOT}/usr/lib/gio/modules/libgioopenssl.so", f"{out}/lib/gio/modules/libgioopenssl.so")
if with_media:
    os.makedirs(f"{out}/lib/gstreamer-1.0", exist_ok=True)
    for n in GST_OURS:
        add_file(f"{GST}/gstreamer-1.0/{n}", f"{out}/lib/gstreamer-1.0/{n}")
    for n in GST_SDK:
        add_file(f"{SYSROOT}/usr/lib/gstreamer-1.0/{n}", f"{out}/lib/gstreamer-1.0/{n}")
while queue:
    f = queue.pop()
    for n in needed(f):
        add_lib(n)

shutil.copy2(f"{DEB}/etc/ssl/ca-certificates.crt", f"{out}/etc/ssl/")
shutil.copy2(f"{DEB}/etc/fonts/fonts.conf", f"{out}/etc/fonts/")
for f in glob.glob(f"{DEB}/share/fonts/*.ttf"):
    shutil.copy2(f, f"{out}/share/fonts/")
shutil.copytree(f"{DEB}/share/X11", f"{out}/share/X11")
if os.path.isdir(f"{STAGE}/share/wpe-webkit-2.0"):
    shutil.copytree(f"{STAGE}/share/wpe-webkit-2.0", f"{out}/share/wpe-webkit-2.0")
with open(f"{out}/run.sh", "w") as f:
    f.write('#!/bin/sh\n# tvf-web (webOS-SDK build): TV loader and glibc, Mali EGL/GLES and Wayland from the TV\n'
            'D=$(cd "$(dirname "$0")" && pwd)\n'
            '# our libraries first (some SDK libraries carry a RUNPATH of /usr/lib)\n'
            'export LD_LIBRARY_PATH="$D/lib"\n'
            + ('# media: WebKit\'s GStreamer code ends in the TVF sinks (encoded data to TVF,\n'
               '# hole for the video plane); registry scan in-process (no gst-plugin-scanner);\n'
               '# no on-disk download buffering (/var/tmp is RAM on the TV, patch 0011);\n'
               '# DASH and HLS straight in <video> as HbbTV pages use them (dashdemux/hlsdemux,\n'
               '# which fetch through WebKit\'s network process)\n'
               'export WEBKIT_GST_QUIRKS=tvf WEBKIT_GST_HOLE_PUNCH_QUIRK=tvf GST_REGISTRY_FORK=no\n'
               'export WPE_SHELL_DISABLE_MEDIA_DISK_CACHE=1\n'
               'export WEBKIT_GST_ENABLE_DASH_SUPPORT=1 WEBKIT_GST_ENABLE_HLS_SUPPORT=1\n'
               if with_media else '')
            + 'exec "$D/bin/tvf-web" "$@"\n')
os.chmod(f"{out}/run.sh", 0o755)
# what TVF may rely on in this build - TVF reads <bundle>/features, one per line:
# video-hole = an element with the attribute tvf-hole (and its own compositing
# layer) punches a hole for the TV's video plane (patch 0008)
# media = WebKit plays media itself (video, MSE, EME) through the TVF sinks
# (patch 0009) - the page needs no media emulation
# eme = EME through TVF's OpenCDM client (libocdm.so): the TV's CDMs, hosted by TVF
features = []
with open(sorted(glob.glob(f"{STAGE}/lib/libWPEWebKit-2.0.so.*"))[0], "rb") as h:
    lib = h.read()
    if b"tvf-hole" in lib:
        features.append("video-hole")
    if b"tvfvideosink" in lib and with_media:
        features.append("media")
    if b"opencdm_construct_session" in lib and with_media:
        features.append("eme")
with open(f"{out}/features", "w") as f:
    f.write("".join(x + "\n" for x in features))
print("Faehigkeiten:", " ".join(features) or "-")
# licences: every file in the bundle belongs to a component; its licence texts go
# to share/licenses/<component>/, the list (version, licence, source) to
# share/licenses/NOTICE.txt. Built here (sources.lock): the texts come from the
# source tarball; from the webOS SDK or the Debian data: ../licenses/sdk/ (see
# its README); ours: the repository's LICENSE.
REPO = os.path.dirname(HERE)
LOCK = {}
with open(f"{HERE}/sources.lock") as h:
    for line in h:
        p = line.split()
        if len(p) >= 3 and not p[0].startswith("#"):
            LOCK[p[0]] = (p[1], p[2])


def pc_version(name):
    for d in (f"{SYSROOT}/usr/lib/pkgconfig", f"{SYSROOT}/usr/share/pkgconfig"):
        try:
            with open(f"{d}/{name}.pc") as h:
                for line in h:
                    if line.startswith("Version:"):
                        return line.split(":", 1)[1].strip()
        except OSError:
            pass
    return "?"


def lib_version(soname):
    """Version from the real file name of a sysroot library (libbz2.so.1 -> 1.0.8), for those without .pc."""
    real = os.path.basename(os.path.realpath(f"{SYSROOT}/usr/lib/{soname}"))
    return real.split(".so.", 1)[1] if ".so." in real else "?"


SDK_NAME = "webOS SDK (arm-webos-linux-gnueabi_sdk-buildroot)"
DEB_NAME = "Debian data files (out/tvf-web-armhf)"
GST_BAD = r"lib/(gstreamer-1\.0/libgst(videoparsersbad|subenc|dash|hls|mpegtsdemux)\.so|libgst(codecparsers|isoff|mpegts|adaptivedemux|uridownloader)-1\.0\.so)"
# name, files in the bundle (regex), licence, origin:
#   ("ours",) | ("lock", sources.lock name, [paths in the source, default: top-level licence files])
#   | ("sdk", text in licenses/sdk, version, where from)
COMPONENTS = [
    ("tvf-web", r"^(bin/tvf-web|run\.sh|features)$", "GPL-3.0-or-later", ("ours",)),
    ("tvf-ocdm", r"^lib/libocdm\.so", "GPL-3.0-or-later", ("ours",)),
    ("gst-tvf", r"/libgsttvf\.so$", "GPL-3.0-or-later", ("ours",)),
    ("wpewebkit", r"^(lib/libWPEWebKit-2\.0\.so|libexec/WPE\w+Process$|libexec/injected-bundle/|share/wpe-webkit-2\.0/|bin/jsc$)",
     "LGPL-2.1-or-later AND BSD-2-Clause, bundled third-party code under its own licences (texts included)",
     ("lock", "wpewebkit", ["Source/WebCore/LICENSE-APPLE", "Source/WebCore/LICENSE-LGPL-2",
                            "Source/WebCore/LICENSE-LGPL-2.1", "Source/JavaScriptCore/COPYING.LIB",
                            "Source/WTF/LICENSE-LLVM.txt", "Source/WTF/LICENSE-dragonbox.txt",
                            "Source/WTF/LICENSE-libc++.txt", "Source/WTF/LICENSE-simde.txt", "Source/WTF/icu/LICENSE",
                            "Source/ThirdParty/ANGLE/LICENSE", "Source/ThirdParty/skia/LICENSE",
                            "Source/ThirdParty/libsysprof-capture/COPYING"])),
    ("brotli", r"^lib/libbrotli", "MIT", ("lock", "brotli")),
    ("nghttp2", r"^lib/libnghttp2", "MIT", ("lock", "nghttp2")),
    ("libpsl", r"^lib/libpsl", "MIT", ("lock", "libpsl")),
    ("libsoup", r"^lib/libsoup-3", "LGPL-2.0-or-later", ("lock", "libsoup")),
    ("glib-networking", r"^lib/gio/modules/", "LGPL-2.1-or-later", ("lock", "glib-networking")),
    ("harfbuzz", r"^lib/libharfbuzz", "MIT (HarfBuzz licence)", ("lock", "harfbuzz")),
    ("libwebp", r"^lib/lib(webp|sharpyuv)", "BSD-3-Clause", ("lock", "libwebp")),
    ("woff2", r"^lib/libwoff2", "MIT", ("lock", "woff2")),
    ("libepoxy", r"^lib/libepoxy", "MIT", ("lock", "libepoxy")),
    ("libgpg-error", r"^lib/libgpg-error", "LGPL-2.1-or-later", ("lock", "libgpg-error")),
    ("libgcrypt", r"^lib/libgcrypt", "LGPL-2.1-or-later", ("lock", "libgcrypt")),
    ("libtasn1", r"^lib/libtasn1", "LGPL-2.1-or-later", ("lock", "libtasn1")),
    ("gst-plugins-base", r"^lib/gstreamer-1\.0/libgstapp\.so$", "LGPL-2.1-or-later", ("lock", "gst-plugins-base")),
    ("gst-plugins-good", r"^lib/gstreamer-1\.0/libgst(isomp4|audioparsers|matroska|id3demux)\.so$",
     "LGPL-2.1-or-later", ("lock", "gst-plugins-good")),
    ("gst-plugins-bad", GST_BAD, "LGPL-2.1-or-later", ("lock", "gst-plugins-bad")),
    ("gcc-runtime", r"^lib/lib(atomic|gcc_s|stdc\+\+)\.so", "GPL-3.0-or-later WITH GCC-exception-3.1",
     ("sdk", "gcc-runtime.txt", "GCC " + subprocess.run([SDKBIN + "gcc", "-dumpversion"], capture_output=True,
                                                         text=True).stdout.strip(), SDK_NAME)),
    ("bzip2", r"^lib/libbz2", "bzip2-1.0.6", ("sdk", "bzip2.txt", lib_version("libbz2.so.1"), SDK_NAME)),
    ("openssl", r"^lib/lib(crypto|ssl)\.so", "Apache-2.0", ("sdk", "openssl.txt", pc_version("openssl"), SDK_NAME)),
    ("expat", r"^lib/libexpat", "MIT", ("sdk", "expat.txt", pc_version("expat"), SDK_NAME)),
    ("libffi", r"^lib/libffi", "MIT", ("sdk", "libffi.txt", pc_version("libffi"), SDK_NAME)),
    ("fontconfig", r"^(lib/libfontconfig|etc/fonts/)", "MIT-style (fontconfig licence)",
     ("sdk", "fontconfig.txt", pc_version("fontconfig"), SDK_NAME)),
    ("freetype", r"^lib/libfreetype", "FTL OR GPL-2.0-or-later",
     ("sdk", "freetype.txt", "libtool " + pc_version("freetype2"), SDK_NAME)),
    ("glib", r"^lib/lib(glib|gio|gobject|gmodule)-2\.0", "LGPL-2.1-or-later",
     ("sdk", "glib.txt", pc_version("glib-2.0"), SDK_NAME)),
    ("gstreamer", r"^lib/(libgst(reamer|base|net)-1\.0|gstreamer-1\.0/libgstcoreelements\.so)", "LGPL-2.1-or-later",
     ("sdk", "gstreamer.txt", pc_version("gstreamer-1.0"), SDK_NAME)),
    ("gst-plugins-base-sdk",
     r"^lib/(libgst(allocators|app|audio|pbutils|riff|rtp|tag|video)-1\.0|gstreamer-1\.0/libgst(typefindfunctions|playback)\.so)",
     "LGPL-2.1-or-later", ("sdk", "gst-plugins-base.txt", pc_version("gstreamer-plugins-base-1.0"), SDK_NAME)),
    ("icu", r"^lib/libicu", "Unicode-3.0 (ICU licence)", ("sdk", "icu.txt", pc_version("icu-uc"), SDK_NAME)),
    ("libjpeg-turbo", r"^lib/libjpeg", "IJG AND BSD-3-Clause AND Zlib",
     ("sdk", "libjpeg-turbo.txt", pc_version("libjpeg"), SDK_NAME)),
    ("pcre2", r"^lib/libpcre2", "BSD-3-Clause WITH PCRE2-exception", ("sdk", "pcre2.txt", pc_version("libpcre2-8"), SDK_NAME)),
    ("libpng", r"^lib/libpng", "libpng-2.0", ("sdk", "libpng.txt", pc_version("libpng16"), SDK_NAME)),
    ("sqlite", r"^lib/libsqlite3", "blessing (public domain)", ("sdk", "sqlite.txt", pc_version("sqlite3"), SDK_NAME)),
    ("libunwind", r"^lib/libunwind", "MIT", ("sdk", "libunwind.txt", pc_version("libunwind"), SDK_NAME)),
    ("xkbcommon", r"^lib/libxkbcommon", "MIT", ("sdk", "xkbcommon.txt", pc_version("xkbcommon"), SDK_NAME)),
    ("libxml2", r"^lib/libxml2", "MIT", ("sdk", "libxml2.txt", pc_version("libxml-2.0"), SDK_NAME)),
    ("zlib", r"^lib/libz\.so", "Zlib", ("sdk", "zlib.txt", pc_version("zlib"), SDK_NAME)),
    ("xkeyboard-config", r"^share/X11/", "MIT-style (xkeyboard-config)", ("sdk", "xkeyboard-config.txt", "Debian", DEB_NAME)),
    ("dejavu-fonts", r"^share/fonts/DejaVu", "Bitstream-Vera AND public domain", ("sdk", "dejavu.txt", "Debian", DEB_NAME)),
    ("ca-certificates", r"^etc/ssl/", "MPL-2.0 (Mozilla CA list)", ("sdk", "ca-certificates.txt", "Debian", DEB_NAME)),
]
LIC_FILE = re.compile(r"(?i)^(copying|license|licence|copyright|notice)([.-][^/]*)?$")


def lock_texts(name, paths, dst):
    """Licence files of a sources.lock entry: the given paths, or the top-level licence files."""
    ver = LOCK[name][0]
    src_dir = f"{HERE}/src/{name}-{ver}"
    got = []
    if os.path.isdir(src_dir) and paths:
        for rel in paths:
            if os.path.isfile(f"{src_dir}/{rel}"):
                t = f"{dst}/{rel.replace('/', '_')}"
                shutil.copy2(f"{src_dir}/{rel}", t)
                got.append(t)
        return got
    tars = glob.glob(f"{HERE}/src/{name}-{ver}.tar.*")
    if not tars:
        return got
    with tarfile.open(tars[0], "r:*") as t:
        for m in t:
            if not m.isfile():
                continue
            rel = m.name.split("/", 1)[1] if "/" in m.name else m.name
            if (rel in paths) if paths else ("/" not in rel and LIC_FILE.match(rel)):
                o = f"{dst}/{rel.replace('/', '_')}"
                with t.extractfile(m) as fi, open(o, "wb") as fo:
                    fo.write(fi.read())
                got.append(o)
    return got


LDIR = f"{out}/share/licenses"
os.makedirs(f"{LDIR}/common", exist_ok=True)
for f in glob.glob(f"{REPO}/licenses/*.txt"):
    shutil.copy2(f, f"{LDIR}/common/")
shutil.copy2(f"{REPO}/LICENSE", f"{LDIR}/common/GPL-3.0.txt")
owner, unknown = {}, []
for root, _, files in os.walk(out):
    for n in files:
        rel = os.path.relpath(os.path.join(root, n), out)
        if rel.startswith("share/licenses/"):
            continue
        c = next((c for c in COMPONENTS if re.search(c[1], rel)), None)
        if c:
            owner.setdefault(c[0], []).append(rel)
        else:
            unknown.append(rel)
notice = ["tvf-web bundle: licences of its parts",
          "",
          "Built by wpe-webos (https://github.com/L-S-D/wpe-webos, GPL-3.0-or-later). Everything",
          "built from source is pinned in its sdk/sources.lock (URL and sha256), WPE WebKit is",
          "patched with sdk/patches/webkit/, the build scripts are in sdk/scripts/. Libraries from",
          "the webOS SDK are its builds of the upstream versions listed. Whoever passes this bundle",
          "on must include this directory and offer the corresponding sources of the LGPL/GPL parts.",
          "Licence texts: <component>/ below; texts the notices refer to (GPL, LGPL, MPL, Apache)",
          "in common/.", ""]
missing_texts = []
for name, _, lic, origin in COMPONENTS:
    if name not in owner:
        continue
    d = f"{LDIR}/{name}"
    os.makedirs(d, exist_ok=True)
    if origin[0] == "ours":
        version, source = "-", "wpe-webos (this build)"
        shutil.copy2(f"{REPO}/LICENSE", f"{d}/LICENSE")
        texts = ["LICENSE"]
    elif origin[0] == "lock":
        version, url = LOCK[origin[1]]
        source = url + (" + sdk/patches/webkit" if origin[1] == "wpewebkit" else "")
        texts = lock_texts(origin[1], origin[2] if len(origin) > 2 else None, d)
    else:
        version, source = origin[2], origin[3]
        shutil.copy2(f"{REPO}/licenses/sdk/{origin[1]}", f"{d}/{origin[1]}")
        texts = [origin[1]]
    if not texts:
        missing_texts.append(name)
    files = sorted(owner[name])
    notice += [f"{name} {version}", f"  licence: {lic}", f"  source:  {source}",
               "  files:   " + ", ".join(files[:6]) + (f" (+{len(files) - 6} more)" if len(files) > 6 else ""),
               f"  texts:   share/licenses/{name}/", ""]
if unknown:
    notice += ["WITHOUT A LICENCE ENTRY (check before passing the bundle on):"] + ["  " + u for u in sorted(unknown)]
with open(f"{LDIR}/NOTICE.txt", "w") as f:
    f.write("\n".join(notice) + "\n")
print("licences:", len(owner), "components in share/licenses")
if unknown or missing_texts:
    print("LICENCE CHECK: no entry for", " ".join(sorted(unknown)) or "-", "| no texts for",
          " ".join(missing_texts) or "-", file=sys.stderr)

# strip everything that is ours (libraries from the SDK sysroot are stripped already)
STRIP = SDKBIN + "strip"
for root, _, files in os.walk(out):
    for n in files:
        f = os.path.join(root, n)
        if os.path.islink(f) or not (n.endswith(".so") or ".so." in n or root.endswith(("/bin", "/libexec"))):
            continue
        with open(f, "rb") as h:
            if h.read(4) != b"\x7fELF":
                continue
        subprocess.run([STRIP, "--strip-unneeded", f], check=False)
print("fehlend:", " ".join(sorted(missing)) or "-")
print("vom TV:", " ".join(sorted(tv)))
print("Bibliotheken:", len(copied))
