# wpe-webos - WPE WebKit as TVF's HbbTV renderer (feasibility)

`tvf-web` runs WPE WebKit in its own process on the TV and renders HbbTV pages on the CPU into shared
memory; TVF is to show the frames as a texture over the live picture and talk to it over a socket
(`PROTOCOL.md`). A WebKit crash cannot take TVF down.

WebKit is not compiled here. The bundle consists of Debian **sid** armhf packages (WPE WebKit 2.54.2 with
the WPE Platform API; trixie's 2.48 is built without it) plus Debian's own loader `ld-linux-armhf.so.3`, so
nothing from the TV's userland (glibc 2.35, softfp ABI) is used. Exact versions: `out/tvf-web-armhf/VERSIONS`.

## Layout

| path | content |
| --- | --- |
| `docker/Dockerfile` | build image `tvf-web-build` (sid, armhf multiarch + cross gcc, amd64 for host tests) |
| `fetch.sh` | builds/updates that image (installs nothing on the host) |
| `helper/tvf-web.c` | the renderer (cJSON from TVF) |
| `helper/tvfca.c` | `libtvfca.so`, preloaded into the WebKit processes: GnuTLS "system trust" = bundled CA file |
| `build-helper.sh` | builds `build/armhf` and `build/amd64` in the image |
| `bundle.py`, `bundle.sh` | dependency closure + data -> `out/tvf-web-<arch>/`, `out/tvf-web-armhf.tgz` |
| `eglprobe/` | EGL/GLES capability probe built with the webOS SDK (for a later GPU path) |
| `tools/tvtest.sh` | test runner on the TV (fetch bundle, render page to PNG with memory watchdog) |
| `tools/proto_client.py` | plays TVF's part of the protocol (host container or the TV's python3) |
| `tools/putrecv.py` | receives files from the TV over HTTP PUT |

Bundle (`out/tvf-web-armhf`, 366 MB, 151 MB as .tgz): `bin/tvf-web`, `lib/` (170 libraries, loader,
GIO TLS module), `libexec/` (WPEWebProcess, WPENetworkProcess, WPEGPUProcess, injected bundle), `etc/fonts`
(fonts.conf: DejaVu + the TV's /usr/share/fonts), `etc/ssl/ca-certificates.crt` (Debian),
`share/glvnd` (Mesa as EGL vendor), `share/X11/xkb`, `run.sh`. Relocatable - tvf-web finds everything
relative to itself.

How it works on the TV:
* WebKit starts its processes from a path compiled into libWPEWebKit
  (`/usr/lib/arm-linux-gnueabihf/wpe-webkit-2.0/`). tvf-web defines `execve`/`posix_spawn(p)` itself and
  redirects those starts to `libexec/` through the bundled loader (`--library-path lib --preload
  libtvfca.so`).
* WebKit 2.54's WebProcess **needs an EGL display even with CPU rendering** (it aborts with "Could not
  create EGL display: no supported platform available"; `WEBKIT_USE_SKIA_FOR_COMPOSITION` does not help).
  So Mesa (surfaceless platform, llvmpipe) is in the bundle - with LLVM. Pages are painted by Skia on the
  CPU (`WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`), frames arrive as SHM buffers.
* tvf-web clears the TV's `LD_LIBRARY_PATH`, `EGL_PLATFORM=wayland`, `GIO_EXTRA_MODULES`, `GST_*` and sets
  its own GIO, fontconfig, glvnd, xkb, CA and cache paths. The TV's `/etc/ld.so.preload`
  (`/lib/libSegFault.so`) only causes a harmless loader warning; softfp libraries of the TV (e.g. NSS
  modules from its nsswitch.conf) are rejected by the hard-float loader, DNS uses glibc's built-in resolver.

## Build and test

```sh
./fetch.sh && ./build-helper.sh && ./bundle.sh            # image, helper, bundles
# host (amd64, bare container = proves the bundle is complete):
docker run --rm -v $PWD/out:/out debian:trixie /out/tvf-web-amd64/run.sh --url https://example.org/ --png /out/x.png
# TV: serve out/ (python3 -m http.server 8765 --bind <PC-IP> --directory out), then
ares-novacom -d <tv> --run 'mkdir -p /media/developer/temp/tvf-web && cd /media/developer/temp/tvf-web &&
  curl -s -o tvtest.sh http://<PC-IP>:8765/tvtest.sh && sh tvtest.sh fetch &&
  sh tvtest.sh run orf http://orfhbbtv.orf.at/orf/redbutton/index.html --keys 403 --settle 12000'
```

For TVF a bundle (e.g. `sdk/out/tvf-web-sdk-media-armhf`) is its own package: TVF's
`./package-web.sh <bundle>` makes `com.lsd.tvf.web_<version>_all.ipk` (bundle under `bundle/`, hidden in
the launcher), `ares-install` puts it next to TVF, which finds it there (start parameter `webBundle`
`auto`; a path still selects any other bundle for tests).

Test mode: `tvf-web --url U --png F [--proxy P] [--keys 403,13,mX:Y,cX:Y,wD:X:Y] [--user-script FILE] [--js EXPR]
[--settle MS] [--timeout MS] [--bg RRGGBB] [--native-keys]` writes the last frame to `F` and the most visible
one to `F-max.png` (composited over `--bg`), prints load/first-frame times and RSS/PSS of all processes.
`TVF_WEB_DEBUG=1|2` for spawn/frame logging.

## Results on the living room TV (2026-11-01)

Measured while the TV was in standby (only CPU 0 of 4 online), TVF playing live TV in parallel, tests at
`nice 19`. PNGs in `out/host/` (`tv-*.png`).

| test | result |
| --- | --- |
| https://example.org | rendered incl. CJK (TV's DroidSansFallback); loaded 1.8 s, first frame 2.0 s after process start (`tv-example.png`) |
| TLS | Debian CA bundle via libtvfca: valid certs OK; self-signed.badssl.com -> `tlsError` flags 1, expired.badssl.com -> flags 8, load refused |
| ORF red button (autostart) | red-button bar over transparent background, first frame 1.8 s (`tv-orf.png`) |
| ORF red key -> portal | key 403 works, portal >50 % visible **1.9 s after the key** (`tv-orfportal-max.png`; with the gateway polyfill as user script) |
| ZDF start | rendered (`application/vnd.hbbtv.xhtml+xml` -> refetch as text/html) (`tv-zdf.png`) |
| ARD start + red | ARD app UI renders; tiles empty without channel data (`tv-ardred-max.png`) |
| proxy | https://ipinfo.io via an HTTP proxy (CONNECT works) shows the proxy's location; direct shows the TV's own |
| protocol (TV's python3 as client) | ready 0.43 s after spawn, first frame 1.5 s, key -> new frame 48-69 ms, resize -> frame 103 ms, 22 fps for a 25 fps ticker, damage rects (16x21 px), exit 0 |
| memory, ORF portal shown | RSS 234 MB, **PSS 181 MB, private 106 MB** (WebProcess 125 MB PSS); MemAvailable stayed > 570 MB |
| JS | **no JIT** in Debian's armhf build (JSC_useJIT=false makes no difference): fib(25) 112 ms, 3M-loop 747 ms, sort of 100k objects 149 ms (amd64 host with JIT: 5 / 28 / 18 ms) |

TV hardware (eglprobe): Mali-G510 (Valhall r46p0), EGL 1.5 with EGL_WL_bind_wayland_display,
EGL_KHR_image_base, EGL_KHR_surfaceless_context, EGL_EXT_image_dma_buf_import, EGL_ANDROID_native_fence_sync,
client EGL_KHR_platform_gbm/wayland; GLES 3.2 with GL_OES_EGL_image(_external), GL_EXT_texture_format_BGRA8888,
GL_EXT_unpack_subimage; max texture 16383. No `/dev/dri` in the app jail. CPU Cortex-A78 x4 (one online in
standby), RAM 1.95 GB (MemAvailable ~560-710 MB with TVF running).

## Open problems

1. Size: 366 MB, of which LLVM 132 MB + z3 16 MB (only for llvmpipe), WebKit 73, ICU data 32, Mesa 29,
   flite voices ~17. A Mesa build with only softpipe (no LLVM) would save ~150 MB; flite could be stubbed.
2. No JavaScript JIT on armhf (Debian builds JSC as interpreter there). Big SPA apps (ZDF, ARD) work but
   are slower; a JIT would need an own WebKit build, if 32-bit ARM JIT is still supported upstream.
3. GPU path (Mali via EGL, zero-copy dma-buf/EGLImage) needs WebKit built against the TV's EGL (softfp,
   TV glibc) - the multi-hour cross build that was avoided here.
4. HbbTV MIME types are handled only for the main document (refetch: second request, POST bodies and
   cookies of that refetch are not WebKit's). Subframes with HbbTV MIME still fail. Alternative: a small
   local proxy in tvf-web that only rewrites Content-Type.
5. Media is off (`--media` exists, but there are no GStreamer plugins in the bundle): A/V objects and
   Mediathek streams must be played by TVF's player, driven by the polyfill.
6. The gateway's polyfill was used unchanged as user script (`window.__HBBTV_FWD = {channel:{}}`); a WPE
   variant should talk to TVF via `window.webkit.messageHandlers.tvf` (channel, broadcast window, keyset,
   exit) instead of `postMessage` to a parent frame.
7. The first start on a TV builds the fontconfig cache (the very first test showed no frame within 2 s);
   keep `--data-dir` persistent or build the cache at install time.
8. Synthetic keys (461, media keys) are `isTrusted=false`; WebKit maps WPE's Back/AudioPlay keysyms to 166/250.
9. `visible` command and `--media` are untested. Debian sid is a moving target - pin via snapshot.debian.org
   for reproducible bundles.

## Licence

wpe-webos' own code (tvf-web, the GStreamer sinks, the OpenCDM client, probes,
build and test scripts) is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version (`LICENSE`, `SPDX-License-Identifier: GPL-3.0-or-later` in each
file). It is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY.

Other parts:

- `sdk/patches/webkit/`: changes to WPE WebKit; each patch is under the licence of
  the files it changes (WebKit: LGPL-2.1-or-later and BSD-2-Clause).
- `helper/cJSON.c`, `helper/cJSON.h`: cJSON 1.7.19, MIT (licence in the files).
- `helper/ocdm/*.h`: OpenCDM client API in the form of Thunder's OpenCDM
  (rdkcentral/ThunderClientLibraries), Apache-2.0 (`licenses/Apache-2.0.txt`).

The tvf-web bundles built here contain WPE WebKit, GStreamer, GLib, libsoup and
other libraries under their own licences (mostly LGPL, MIT, BSD); `sdk/sources.lock`
pins the sources built from, the webOS SDK provides the rest.
`sdk/scripts/bundle-sdk.py` puts their licence texts into the bundle's
`share/licenses/` (from the source tarballs, for the SDK's libraries and the Debian
data files from `licenses/sdk/`) and lists every component with version, licence,
source and files in `share/licenses/NOTICE.txt`; a file it cannot assign is listed
there and reported. Whoever passes a bundle on must keep that directory and offer
the corresponding sources of the LGPL/GPL parts.
