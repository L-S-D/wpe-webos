# WPE WebKit built with the webOS SDK (Mali GPU, JavaScript JIT)

Second way to build `tvf-web`: instead of Debian's armhf userland (own loader, Mesa llvmpipe, JavaScript
interpreter only) WebKit 2.54.2 is cross-compiled with the webOS SDK (GCC 14.2, armv7-a, **softfp**, glibc
2.12 headers). It then runs with the TV's own loader and glibc and renders on the TV's Mali-G510 through
the TV's EGL/GLES. JavaScriptCore runs as interpreter (LLInt): 2.54 has no 32-bit JIT (see below).
tvf-web and `../PROTOCOL.md` are unchanged. Status 2026-11-02: renders correctly on the living room TV,
about twice as fast to the first frame and with ~45 % less memory than the Debian bundle (results below).

## Layout

| path | content |
| --- | --- |
| `docker/Dockerfile` | host tools (cmake, ninja, meson, ruby, perl, python3, gperf, unifdef, bison, glib tools); the SDK is mounted read-only |
| `sources.lock`, `scripts/fetch-sources.sh` | all sources with pinned sha256 (WebKit and GNOME sums cross-checked with upstream) |
| `cross/` | CMake toolchain file and meson cross file for the SDK |
| `scripts/run.sh` | runs a command in the container: `nice -n 19 ionice -c3`, `JOBS` (default 3); `WEBOS_SDK_HOST` = SDK elsewhere on the host; as non-root it runs as that user |
| `scripts/build-deps.sh` | brotli, nghttp2, libpsl, libsoup 3, glib-networking (OpenSSL), harfbuzz, libwebp, woff2, libepoxy, libgpg-error, libgcrypt, libtasn1 into `sysroot/` (a writable copy of the SDK sysroot); on request the GStreamer plugins of the media build into `stage-gst/` |
| `scripts/build-webkit.sh` | unpack, apply `patches/webkit/*.patch`, configure, build, install (`stage/` and `sysroot/`); `MEDIA=1` = the media build (`build/webkit-media`, `stage-media/`); changed patches are synced into the unpacked tree file by file |
| `scripts/build-helper.sh` | tvf-web against that WebKit (`$ORIGIN/../lib` as rpath) |
| `scripts/build-gst-tvf.sh`, `../helper/gsttvf.c` | the TVF GStreamer sinks of the media build (below) into `stage-gst/` |
| `scripts/bundle-sdk.py` | bundle: everything except glibc, EGL/GLES and Wayland, which come from the TV; `--media`: the media build plus GStreamer plugins |
| `wsl/` | building on a faster machine in the LAN (WSL2 + Docker): `pack.sh full\|update` packs, serves and receives the bundle back (below) |
| `tests/mse/` | offline MSE check (dash.js) of the media build under qemu |
| `compat/glibc-compat.h` | force-included: constants the SDK's glibc 2.12 / Linux 3.10 headers lack |
| `patches/webkit/` | WebKit patches (below) |
| `probe/eglprobe2.c` | offscreen Mali EGL test in a child process |
| `probe/pcstest.cpp` | 40-line proof of the Skia stage ABI bug (patch 0006), runs under qemu |
| `docker-qemu/`, `out/qemu/run-tests*.sh` | offline tests under qemu-arm with Debian armel Mesa (below) |
| `../tools/tvrun-sdk.sh` | TV tests: `fetch`, `fetchdeb`, `run sdk\|deb NAME URL [args]`, `proto`, `jsc` |

```sh
./scripts/fetch-sources.sh
docker build -t tvf-web-sdkbuild docker
./scripts/run.sh sh scripts/build-deps.sh
./scripts/run.sh sh scripts/build-webkit.sh all       # hours
./scripts/run.sh sh scripts/build-helper.sh
python3 scripts/bundle-sdk.py out/tvf-web-sdk-armhf

# media build (video, MSE, EME through the TV's decoder, below) - same deps plus:
./scripts/run.sh sh scripts/build-deps.sh gst_plugins_base gst_plugins_good gst_plugins_bad
./scripts/run.sh sh scripts/build-gst-tvf.sh
# (builds TVF's OpenCDM client first, scripts/build-ocdm.sh - EME, see below)
MEDIA=1 ./scripts/run.sh sh scripts/build-webkit.sh all  # or on a faster machine: sh wsl/pack.sh full
python3 scripts/bundle-sdk.py out/tvf-web-sdk-media-armhf --media
```

## Findings before the build

**Float ABI and JIT - showstopper for the JIT.** The SDK targets `arm-webos-linux-gnueabi`:
`-mfloat-abi=softfp -mfpu=neon-fp16 -march=armv7-a -mcpu=cortex-a9`; the TV's libraries incl. `libmali.so` are
soft-float (ELF flags 0x5000200), so a process that uses the TV's EGL/GLES must be softfp. JavaScriptCore
2.54 has **no JIT on 32-bit at all**: `WTF/wtf/PlatformEnable.h` does `#if USE(JSVALUE32_64) #undef
ENABLE_JIT / #define ENABLE_JIT 0`, whatever CMake says (CMake still reports `ENABLE_JIT ON` for ARM
Thumb-2 Linux; `Options::useJIT` defaults to `isAddress64Bit()` and stays false even when forced). This
was found only after the build (checked CMake and `CCallHelpers.h`, not this header - my mistake). 2.52 was
the last release with the ARMv7 JIT, and there only for `CPU(ARM_THUMB2) && CPU(ARM_HARDFP) && OS(LINUX)`;
`CCallHelpers.h` says "ARM (hardfp, which we require)". So: 2.54 = interpreter only (LLInt in assembly, not
the C loop); a JIT would need 2.52 plus a patch that allows softfp - Baseline only (LLInt and Baseline call
C++ with integer/JSValue arguments only; doubles go to C++ only from the DFG, WebAssembly and the math
thunks, which are not generated on Linux ARMv7), DFG off, patch 0002 as guard. Untested, another ~7 h build,
and 2.52 no longer gets security fixes.

**EGL display.** The 2.54 WebProcess tries GBM (needs a DRM render node - none in the app jail), then
`EGL_MESA_platform_surfaceless` (Mali has no such extension), then the default display - but that last
step only exists for GTK and Android. `probe/eglprobe2.c` on the living room TV, in a forked child of the
dev shell: `eglGetDisplay(EGL_DEFAULT_DISPLAY)` works (also without `XDG_RUNTIME_DIR`/`EGL_PLATFORM`),
surfaceless context + FBO + `glReadPixels(GL_BGRA_EXT)` give correct pixels, 1280x720 clear + read back
takes 6.6-7.3 ms. The Wayland display works as well (needs `XDG_RUNTIME_DIR`). Patch 0001 enables the
default display for WPE (Wayland as fallback, loaded with dlopen) and lets that display render into GL
renderbuffers that are read back into the shared-memory buffers the UI process gets.

**Libraries.** SDK sysroot: glib 2.82.5, ICU 73.2, libxml2 2.13.6, sqlite 3.48, freetype 2.13.3, fontconfig
2.16.0, OpenSSL 3.4.1, xkbcommon 1.7.0, libpng 1.6.46, libjpeg-turbo 2.1.5 - all above WebKit 2.54's
minimums (glib 2.70, ICU 70.1, libxml2 2.9.13, freetype 2.9). Missing and built here: harfbuzz, libsoup 3,
libpsl, nghttp2, brotli, glib-networking, libwebp, woff2, libepoxy, libgcrypt/libgpg-error, libtasn1. The
SDK's glibc is 2.12 (binaries need nothing newer, the TV has 2.35). From the TV at run time: glibc,
`libEGL`/`libGLESv2` (stubs for `libmali.so`), `libwayland-*`. Mali itself pulls in the TV's
`libglib-2.0.so.0` (2.72), `libffi.so.8`, `libstdc++.so.6` (GCC 11) and `libgcc_s` - the bundle has newer
copies with the same sonames, loaded first (`LD_LIBRARY_PATH`), so each library exists once per process.

## Patches (`patches/webkit/`)

* `0001-wpe-vendor-egl-default-display.patch` - `PlatformDisplayDefault` also for WPE: default EGL display,
  Wayland display as fallback; WebProcess falls back to it after GBM/surfaceless; swap chain for that
  display = `SharedMemory` (GL rendering, `glReadPixels` into SHM); webkit://gpu names the platform.
* `0002-jsc-armv7-softfp-guard.patch` - on softfp ARM a `double` argument to a C call stops with a clear
  message instead of passing it in the wrong registers; `double` results are taken from r0:r1. Without
  effect in 2.54 (no 32-bit JIT), kept for a 2.52 attempt.
* `0003-shared-memory-memfd-without-linux-memfd-h.patch` - shared memory via the memfd syscall although
  the SDK's 3.10 kernel headers have no `linux/memfd.h` (otherwise `shm_open` = writable `/dev/shm`).
* `0004-wpe-backing-store-without-libdrm.patch` - `AcceleratedBackingStore.cpp` used `DRM_FORMAT_*`
  without including anything when built without libdrm (upstream bug); takes the kernel header.
* `0005-tvf-diagnostics.patch` - switches, all off by default: `TVF_WEB_GL_DEBUG=1` (GL renderer per
  context, `KHR_debug` messages, FBO/Skia render target status, which EGL context each step runs in, and
  for the first 12 frames what the readback produced: visible/non-white/changed samples, first pixel,
  `glGetError`), `TVF_WEB_TESTPATTERN=1` (red/green/blue bars drawn into the frame just before the
  readback), `TVF_WEB_EGL_DEFAULT=1` (skip the surfaceless display, for offline tests with Mesa).
* `0006-skia-raster-pipeline-gcc-softfp-stage-abi.patch` - **the cause of the blank frames** (next
  section): Skia's raster pipeline stage ABI only with clang or hardfp.
* `0007-shm-readback-glreadpixels-bgra.patch` - Skia composition + shared memory: read the frame with
  `glReadPixels(GL_BGRA)` straight into the SHM buffer instead of `SkSurface::readPixels()` (RGBA into a
  temporary buffer, then a CPU conversion pass); RGBA + swap if the driver has no BGRA readback.
  `TVF_WEB_SKIA_READBACK=1` selects Skia's path again.
* `0008-tvf-video-hole.patch` - the video hole of a TV browser without media support in WebKit
  (`ENABLE_VIDEO` stays off, no GStreamer): an element with the attribute `tvf-hole` and its own
  compositing layer (TVF's media.js sets `will-change: transform`) gets WebKit's hole punch buffer as
  layer contents (`RenderLayerBacking`). The compositor clears its content box (Skia: `kClear`,
  TextureMapper: transparent without blending), so everything composited below it becomes transparent
  - the TV's video plane shows through - and everything above it stays. `Element::attributeChanged`
  reconfigures the layer when the attribute changes. The hole punch buffer, its early-release exception
  and both compositors' hole paths were behind `ENABLE(VIDEO)`. `bundle-sdk.py` writes
  `<bundle>/features` (`video-hole`, found by the attribute name in `libWPEWebKit`); TVF reads it and
  falls back to its clip-path approximation without it. Partial rebuild: ~10 steps, a few minutes.
* `0009-gstreamer-quirk-tvf.patch` - media build only: GStreamer quirks `tvf` after WebKit's Rialto
  model (`WEBKIT_GST_QUIRKS=tvf`, `WEBKIT_GST_HOLE_PUNCH_QUIRK=tvf`, set by the bundle's `run.sh`).
  `GStreamerQuirkTVF` merges the TVF sinks' caps into uridecodebin3's `caps` (decodebin3 stops at the
  still encoded streams), takes `tvfaudiosink` as audio sink and sets the playbin flags
  text/native-audio/native-video; `GStreamerHolePunchQuirkTVF` makes `tvfvideosink` the video sink of
  every video and hands it the video rectangle (`rectangle` = "x,y,w,h"), while the compositor punches
  the hole as in 0008. In parsebin a parsed/framed stream the sinks take is exposed without a second
  parser (`autoplug-select`), which kept rewriting the time stamps of the first B-frame pictures.
  WebKit's in-memory buffering counts as full (`correctBufferingPercentage`, as the Broadcom quirk adds
  its decoder's data): the sinks drain the small queue in bursts whenever the TV takes data, and
  WebKit paused and resumed a progressive MP4 several times a second until it hung.
* `0010-gstreamer-gl-mpegts-optional.patch` - `GStreamerChecks.cmake` required the GStreamer GL and
  MPEG-TS libraries, which the SDK lacks; only when `USE_GSTREAMER_GL`/`USE_GSTREAMER_MPEGTS` are on.
* `0011-media-disk-cache-switch-for-download-buffer.patch` - `WPE_SHELL_DISABLE_MEDIA_DISK_CACHE=1` also
  keeps urisourcebin from downloading a progressive file with `preload=auto` into a temp file
  (`/var/tmp`, RAM on the TV): on the living room TV a 1.7 GB ZDF MP4 went in at ~25 MB/s until webOS
  closed TVF. The media bundle's `run.sh` sets the variable; the TV decoder buffers anyway.
* `0012-webkitwebsrc-range-answered-with-200.patch` - WebKit's HTTP source stops a download when its
  queue is full and resumes it with a range request; ORF's DASH CDN answers that with the whole
  segment (200, despite `Accept-Ranges: bytes`) and playback failed ("unexpected 200 HTTP status code
  for range request"). Now the bytes before the requested position are dropped and the resource is
  not stopped again.
* `0013-thunder-cdm-hardware-robustness.patch` - the Thunder CDM offered only `""`,
  `SW_SECURE_DECODE` and `SW_SECURE_CRYPTO`, so pages asking for Widevine `HW_SECURE_ALL` or PlayReady
  `3000` (dash.js' levels for `com.microsoft.playready.recommendation`) got no key system access - the
  TV's CDMs are hardware ones (Widevine L1, PlayReady with the TEE).
* `0014-hole-punch-buffer-for-recreated-layers.patch` - a GStreamer player pushes its hole punch
  buffer once per video sink, and the layer it went to keeps it. When the video's layer is recreated
  (arte: `display:none` on the container and back), the new layer got no contents: no hole, and the
  sink never got a rectangle - sound and subtitles, no picture. The buffer proxy now keeps a copy of
  the last hole punch buffer and gives one to every layer that attaches later.
* `0015-mixed-content-loopback.patch` - loopback (127.0.0.0/8, ::1, localhost) is not mixed content,
  for every destination: WebKit let it through only for upgradable media (images, audio, video) and
  blocked scripts and fetches from `http://127.0.0.1` on https pages - TVF's relay serves dash.js
  for the OIPF replacement there. Mixed Content Level 2 and Chromium do the same; other `http://`
  hosts stay blocked. Check: `tests/mixed/run-qemu.sh` (https page with an own CA under qemu).

`tvf-web` itself: `TVF_WEB_CPU=1` = Skia CPU painting (`WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`), still GPU
composition; `WEBKIT_USE_SKIA_FOR_COMPOSITION=0` = WebKit's TextureMapper compositor instead of Skia.

## Blank frames on the TV (2026-11-02): cause and fix

First TV run: GL on Mali worked (webkit://gpu: Mali-G510, GLES 3.2), but every frame came back empty -
opaque pages 100 % white, transparent ones 100 % transparent - also with CPU painting.

Cause: Skia's CPU raster pipeline. On 32-bit ARM with NEON, `SkRasterPipeline_opts.h` declares its stage
functions with `__attribute__((pcs("aapcs-vfp")))` so that the r,g,b,a vectors travel in q0-q3. GCC 14
(the SDK's compiler, `-mfloat-abi=softfp`) applies the attribute to the stage definitions but not to the
calls through the `Stage` function pointer: the caller passes r in r2/r3 and g,b,a on the stack, the callee
reads q0-q3 (`probe/pcstest.cpp`: `0.5 0 0 0 ...` instead of `1.5 2.5 3.5 4.5 ...`; the assembly shows it
directly). Every raster pipeline run therefore produces garbage. Skia only builds with clang upstream, and
Debian's armhf build (hardfp, no NEON -> scalar pipeline) never takes that path. Affected here:
* `SkSurface::readPixels()` - converts the RGBA read from the GPU into the BGRA SHM buffer with the
  raster pipeline: every frame of the GPU path came back as a constant colour (white / transparent on
  Mali, stripes under qemu). This was the blank screen.
* CPU painting (`TVF_WEB_CPU=1`), unaccelerated 2D canvas, any CPU pixel conversion: solid fills and
  text partly fine (legacy blitters), images and gradients broken (stripes, yellow blocks on the ORF
  portal).
Fix (0006): without clang and without hardfp the stages use the base ABI (vectors on the stack) - correct,
somewhat slower on the CPU path. 0007 additionally avoids the conversion pass for every frame.

How it was found: offline under qemu with Debian's armel Mesa (llvmpipe) as GL driver (works once the
bundle's own `libstdc++`/`libgcc_s` are left out): TextureMapper composition rendered correctly, Skia
composition did not even show test bars drawn into the frame right before the readback, Debian's build
rendered the same page fine -> readback conversion -> raster pipeline -> stage ABI.

Verified on the TV (living room, 2026-11-02): test page with text, JPEG, PNG with alpha, gradient layer and
2D canvas - GPU path, Skia readback (`TVF_WEB_SKIA_READBACK=1`) and CPU painting give the same picture (0
pixels differ beyond 3 % rounding) and the same as qemu; example.org shows its text (incl. Arabic, CJK,
Cyrillic), the ORF portal its images, ZDF its Bundesliga teaser; PSNR against the Debian bundle's PNGs
43-67 dB.

## Results on the living room TV (2026-11-02)

TV in standby (2 of 4 CPUs online), TVF playing RTL HD in parallel, tests at `nice 19`, SDK and Debian
bundle alternating, two rounds (first round cold network cache). Times from process start.

| test | SDK bundle (Mali GPU, Skia GL) | Debian bundle (llvmpipe, CPU painting) |
| --- | --- | --- |
| example.org first frame (loaded) | 749 / 442 / 444 ms (889 / 437 / 365) | 1692 / 1059 ms (1378 / 712) |
| ORF red button first frame | 688 / 566 / 626 ms | 1426 / 1218 ms |
| ORF red key -> portal > 50 % visible | 776 / 752 / 780 ms | 873 / 739 ms |
| ZDF start first frame | 503 / 465 ms | 1118 / 779 ms |
| PSS total (private), example.org | 100 MB (51) - WebProcess 68 | 189-201 MB (113-124) - WebProcess 129 |
| PSS total (private), ORF portal | 114 MB (62) - WebProcess 82 | 193-201 MB (114-122) - WebProcess 138 |
| PSS total (private), ZDF | 119-120 MB (66-67) - WebProcess 84 | 209-216 MB (128-134) - WebProcess 145 |
| MemAvailable drop during the run | 64-77 MB | 112-148 MB |
| protocol: ready / first frame (640x360) | 120-154 / 388-570 ms | 239-247 / 851-1352 ms |
| protocol: key -> new frame, resize -> frame | 28-41 ms, 47-48 ms | 25-50 ms (once 243), 51-52 ms |
| protocol: frames/s (25 fps ticker) | 24 | 24 |
| JS in a page (fib 25 / 3M loop / sort 100k objects) | 70 / 450 / 87 ms | 99-124 / 534-581 / 92-95 ms |
| `jsc` fib(25), `jit-check.js` | 76 ms, 755 ms; results identical to V8 on amd64 | (no jsc in the bundle) |
| CPU painting (`TVF_WEB_CPU=1`) | example.org first frame 702-746 ms, ORF key -> visible 669 ms, PSS 104-110 MB | - |
| bundle | 120 MB, 58 MB .tgz | 366 MB, 152 MB .tgz |

PSS does not include what the Mali driver allocates on the GPU side; MemAvailable covers that. Both
bundles show the same page behaviour (ZDF: first load "Frame load interrupted" = HbbTV MIME refetch; ORF
portal: last frame transparent because the portal closes again - the `-max.png` frame is the one to look
at). `libmali` is mapped in the SDK WebProcess (4 mappings), not in the Debian one.

`compat/glibc-compat.h` (force-included): `MADV_DONTDUMP`/`MADV_DODUMP` (bmalloc), `MFD_*` and ARM
syscall numbers newer than the SDK's headers (`memfd_create` 385, `getrandom` 384, `membarrier` 389,
`statx` 397, `renameat2` 382).

## Build (2026-11-01)

WebKit options (`scripts/build-webkit.sh`): WPE Platform with the headless backend only (no DRM/Wayland
platform, no legacy libwpe API); JIT requested but compiled out by WTF on 32 bit (see above), DFG/FTL/
WebAssembly off, C loop off (LLInt in assembly); media (video, web audio, media stream/recorder, web codecs,
EME), GStreamer, WebGL, GPU process, gamepad, WebXR, Vulkan, speech, ATK, spell check, hyphenation, XSLT,
PDF.js, AVIF, JPEG XL, LCMS, GBM/libdrm, bubblewrap, journald, libbacktrace, sysprof, introspection, docs,
MiniBrowser, tests and WebDriver off; kept: WOFF2, WebP, JPEG, PNG, Skia (GPU via GLES), remote inspector,
HTTP/2 (nghttp2), TLS (glib-networking with OpenSSL).

Time at `-j3`, `nice -n 19 ionice -c3` on the Intel N100 while TVH was streaming: dependencies ~15 min,
WebKit ~7 h 20 min (42 min + 6 h 36 min + final links; two restarts: compat header and `USE_GSTREAMER=OFF`).
Every compiler process needed 1.3-2.9 GB.

Bundle `out/tvf-web-sdk-armhf` (`scripts/bundle-sdk.py`, stripped): **120 MB, 58 MB as .tgz** (Debian
bundle: 366 MB / 152 MB) - `libWPEWebKit-2.0.so.1` 48.8 MB, ICU data 30.6 MB, 45 libraries, `bin/jsc`
9.5 MB (test shell, can be left out). From the TV at run time: glibc (`ld-linux.so.3`, `libc`, `libm`,
`libpthread`, `librt`, `libdl`, `libresolv`); `libEGL.so.1`/`libGLESv2.so.2` are opened by libepoxy at run
time. `run.sh` sets `LD_LIBRARY_PATH` (some SDK libraries carry a RUNPATH of /usr/lib).

## Offline checks (no TV)

`docker-qemu/`: qemu-arm with Debian's armel glibc as stand-in for the TV's (same soft-float calling
convention; the SDK's own glibc 2.12 crashes under qemu).
* `jsc` from the build runs; `tests/jit-check.js` (doubles, Math, typed arrays, sorting, formatting) and
  a RegExp check give identical results with `--useJIT=true/false` - and `--dumpOptions` shows the JIT
  stays off, which is how the 32-bit JIT removal showed up.
* `tvf-web` starts, the WebKit processes start from the bundle, all libraries resolve. Found and fixed
  here: libgpg-error had generated a zero-size lock object (configure needs the SDK `objdump` on PATH and
  gawk; mawk cannot read hex) - every WebKit process aborted with `gpgrt fatal: sizeof lock obj`.
* GL path offline: Debian's armel Mesa needs armel-versioned libstdc++ symbols (`GLIBCXX_3.4.23` for
  `std::__future_base`), our ARMv7 libstdc++ has them at 3.4.15 - so the qemu copy of the bundle
  (`out/tvf-web-sdk-qemu`) leaves out `libstdc++`/`libgcc_s` and uses Debian's; then WebKit renders
  through llvmpipe (`EGL_PLATFORM=surfaceless`, `TVF_WEB_EXEC_PREFIX="qemu-arm -cpu cortex-a15"` so that
  the child processes run under qemu too). Slow (first frame ~5-10 s), results match the TV. For the TV
  it was checked by symbols: all 178 versioned libstdc++ symbols `libmali.so` imports exist in the
  bundled libstdc++, likewise for `libEGL`, `libGLESv2` and `libwayland-client`.
* A `file://` HTML page was shown as plain text under qemu (probably because the bundle has no
  shared-mime-info database; not checked further) - the tests use http or `data:text/html;base64,...`.
* Video hole (patch 0008, 2026-11-02): `tests/hole/run.sh` renders four pages (hole from the start,
  attribute set later, removed later, control) - the hole covers exactly the element's box minus an
  overlay above it, the foreign element below it is cleared, removing the attribute removes the hole.
  The first build of the patch showed no hole: 2.54 composites with Skia (`SkiaCompositingLayer`), whose
  hole branch was behind `ENABLE(VIDEO)`. With TVF's media.js on an RTL-like page (fullscreen video
  above a programme list) only the control bar stays visible.
* MSE (media build, 2026-11-02): `tests/mse/run.sh` (content from `tests/mse/make-content.sh`), results
  in the next section.
* Pointer and wheel (2026-11-03): `tests/pointer/run-qemu.sh` drives tvf-web's test mode (`--keys` with
  `mX:Y`, `cX:Y`, `wD:X:Y`) against a page with a button and a scroll list: mouseover, mousedown
  (`buttons=1`, `detail=1`), mouseup, click with `:hover`, wheel ±80 px on the element under the pointer.
  WebKit coalesces wheel steps that arrive at once into one event.

## Media: WebKit plays, the TV decodes (2026-11-02)

Goal: HbbTV pages that use `<video>`, MSE (dash.js, Shaka) and later EME run as they do in the TV's own
HbbTV browser - without TVF's media emulation in JavaScript. Model: WebKit's "Rialto" pattern - WebKit
does all the media logic (MSE byte stream parsing, buffering, seeking, A/V tracks), its GStreamer
pipeline ends in sinks that take the still encoded streams, and a media server (TVF, stage 2) feeds them
to the TV's hardware decoder; the video plane shows through the compositor's hole punch.

* WebKit: `MEDIA=1` = `ENABLE_VIDEO`, `USE_GSTREAMER`, `ENABLE_MEDIA_SOURCE`, `ENABLE_ENCRYPTED_MEDIA`
  (GStreamer GL/MPEG-TS off, patch 0010), quirks from patch 0009. Own build and stage directory; the
  default build stays as it is.
* GStreamer: the SDK has 1.24.11 with the base libraries and coreelements, typefindfunctions and
  playback (playbin3, decodebin3, parsebin, urisourcebin). Built here into `stage-gst/` (not the
  sysroot, WebKit compiles against the SDK's headers): app (appsrc/appsink of the MSE append
  pipeline), isomp4 (qtdemux), audioparsers, matroska, videoparsersbad (h264parse/h265parse), subenc
  (webvttenc for WebKit's text combiner and MSE text tracks; closedcaption/CEA-608 would need
  pangocairo).
* `tvfvideosink`/`tvfaudiosink` (`helper/gsttvf.c`): klass `Decoder/.../Sink/...` and rank MARGINAL+1, so
  WebKit's registry scanner counts them as decoders (`isTypeSupported`, `canPlayType`). They take
  H.264/HEVC as byte-stream or length-prefixed (avc/avc3, hvc1/hev1 - what MSE delivers from MP4: the
  append pipeline's h264parse keeps avc, HEVC has no parser there) and AAC raw or ADTS, MPEG audio,
  AC-3/E-AC-3, and turn everything into what the TV's ES input takes: byte-stream with the parameter
  sets from `codec_data` in front of each key frame (after an AUD), AAC with an ADTS header per frame.
  `rectangle`, `volume`/`mute` (GstStreamVolume, else playsink looks for a `volume` element); they
  never drop (WebKit sets `max-lateness` 100 ms with QoS on video sinks). With TVF (`TVF_MEDIA_SOCK`)
  they send the access units with their stream time to TVF's media server (`../PROTOCOL-MEDIA.md`):
  sync off, backpressure only through credit (they wait on a condition, never on the socket), the
  position query answered from the TV, EOS held until the TV has shown the last frame. PLAYING ->
  PAUSED is synchronous then: the TV holds the picture, and waiting for a new buffer deadlocked when
  one sink had passed on everything and its next data sat in the demuxer behind the paused other
  stream (a render waiting for credit is woken first: `set_async_enabled` takes the preroll lock it
  holds; waiting itself uses `wait_preroll`, which counts as prerolled). Without TVF
  they only log (`TVF_SINK_LOG=all`) and dump (`TVF_SINK_DUMP=<dir>`, one file per start).
* `tvf-web --media` switches media on (without it WebKit creates `<video>` as an unknown element),
  together with MSE and the EME API; autoplay with sound is allowed (website policy), otherwise WebKit
  pauses a page's video as soon as it unmutes it.
* Bundle: `bundle-sdk.py OUT --media` adds the plugins, sets the quirk variables in `run.sh` and writes
  `media` into `<bundle>/features`.

Stage 1 under qemu (`tests/mse`, dash.js 4.7.4, 20 s H.264 High 720p25 with B-frames + AAC-LC as DASH):
`isTypeSupported` true for avc1/mp4a/hev1/ec-3/ac-3, false for VP9; `canPlayType` "probably"; dash.js
plays in real time to the end, seeking to 12 s starts a new segment with a key frame; the video sink gets
the element's rectangle (100,100,640,360) and the compositor clears exactly 640x360 pixels; volume/mute
arrive. The video stream the sink produced decodes to all 500 frames bit-identical to the source in the
same order (ffmpeg framemd5), the AAC as ADTS cleanly (MSE drops the AAC priming frame - append window).

Stage 2 offline (2026-11-02): `tests/mse/tvf-mock.py` stands in for TVF (a decoder that plays in real
time, takes data 2 s ahead, reports the position and the end); `page.sh` with
`TVF_MEDIA_SOCK=@tvf-media-test` and `URL=.../mse.html?pause&seekplay&noseek`: the pause holds
currentTime, the seek from 14 s back to 6 s while playing starts a new epoch at the key frame, `ended`
fires at 20.00 s when the "TV" has shown the last frame; video and audio form one session, the
outstanding data never exceeds the window. The same run against TVF's real media server
(`src/app/webmedia.c`) with a simulated decoder: TVF `tools/webmedia-test`.

On the living room TV (2026-11-02, screen off, `WEBMEDIA 1`): the MSE page plays on the TV's decoder
(load 220-280 ms, pause holds, seeking while playing reloads in ~0.5 s, the TV reports the real end);
a ZDF MP4 1080p50 (1.7 GB, WebKit's regular path) starts at once, seeks, pauses and resumes, with
`/var/tmp` untouched and the WebProcess at ~180 MB.

DASH and HLS straight in `<video src>` (HbbTV pages such as ORF ON set an MPD): `dashdemux`/`hlsdemux`
from gst-plugins-bad (libxml2, OpenSSL for AES-128 HLS) plus `tsdemux` and `id3demux`, enabled with
`WEBKIT_GST_ENABLE_DASH_SUPPORT=1 WEBKIT_GST_ENABLE_HLS_SUPPORT=1` in the bundle's `run.sh`. WebKit
keeps the adaptivedemux2 demuxers at rank NONE (they fetch with their own libsoup, past WebKit's
network stack); these older ones fetch every segment through WebKit's own source, i.e. the network
process with its proxy (TVF's relay and its country rules) and cookies. Under qemu: a local MPD, a
public DASH stream (HTTPS) and a public HLS stream with TS segments play, seek and pause; an ORF ON
programme needed patch 0012. On the living room TV (2026-11-02): ORF ON as DASH starts at 512x288 and
switches up to 1080p50 in the running stream (the TV's decoder follows without a reload), seeks to
20 min in ~3 s, pauses and resumes; the HLS stream (TS, 60 fps) likewise.

Open:
* WebKit's `buffered` for a progressive file shows the whole duration (the quirk reports the buffer as
  full); real underruns only show as a standing picture, no `waiting`.

### EME (2026-11-02)

2.54's GStreamer port gets key systems only through Thunder (OpenCDM; no ClearKey any more). The TV's
own DRM modules are Chromium "library CDMs" (`CreateCdmInstance`, `InitializeCdmModule_10`):
`/usr/lib/libwidevine-wrapper.so` (Widevine, through `com.webos.service.widevine`) and
`/usr/lib/libplayreadycdm.so` (PlayReady). In LG's webOS variant `Decrypt()` takes the stream type and
returns the sample for the secure video path (Widevine: the sample with serialized metadata appended,
`libsvp_inband_serializer`), which goes unchanged into a BUFFERSTREAM pipeline loaded with
`option.drm.type` and `externalStreamingInfo.svpVersion` - how LG's browser does it, and what Kodi's
inputstream.adaptive does on webOS (`TARGET_WEBOS`), the public reference for all of this.

So, Rialto's split again: `MEDIA=1` builds WebKit with `ENABLE_THUNDER` against TVF's OpenCDM client
(`helper/ocdm/*.h`, `helper/tvf-ocdm.c` -> `libocdm.so`, `scripts/build-ocdm.sh`, run by
`build-webkit.sh`; FindThunder takes any library named `ocdm`, `ocdmRialto` or `ClientOCDM`). The
client forwards key system queries, sessions, license requests and updates to TVF, which hosts the
TV's CDMs (TVF `src/app/cdm.cpp`, `webdrm.c`). WebKit's decryptor does not decrypt: the client tags
each encrypted sample with its session (GstProtectionMeta `application/x-tvf-cenc`), the sinks send
it with IV, key id and subsample map (corrected for the start codes, parameter sets and ADTS headers
they insert), and TVF feeds what its CDM makes of it. The sinks also look upstream for the
`application/x-cenc` caps in front of WebKit's decryptor, so TVF loads the TV pipeline for DRM before
the first encrypted sample (clear lead). Protocol: `../PROTOCOL-MEDIA.md`, "EME". Patch 0013 adds
the hardware robustness levels. The bundle's `features` says `eme`.

`build-webkit.sh` now reconfigures an existing tree when the configure options change (they are
hashed into `<build>/.tvf-opts`) - the Windows tree was configured without Thunder.

On the living room TV (TVF's `CDMTEST`, without WebKit): Widevine works from the app - the wrapper
(17.1.2) creates interface 10, Shaka's "Angel One" gets its license (5 keys usable), and `Decrypt()`
returns the sample unchanged behind a 76-byte header (`ibpi`, version 2, clear/encrypted sizes, IV,
key id) for the pipeline's Widevine decryptor (it selects the key through the OEMCrypto session).
The host has to behave like inputstream.adaptive's: no storage id (with one the wrapper treats the
app as a new, unprovisioned origin and rejects `generateRequest`), no distinctive identifier or
persistent state; the wrapper resolves the create promise twice (with and then without the session
id); clear samples only pass as cenc with a zero IV and one all-clear subsample. PlayReady does not
work inside an app: the porting kit needs the secure partition (`/dev/mmcblk0p26`) and root-only
devices for its TEE proxy (`Drm_Initialize` fails), and there is no service like Widevine's - so
TVF does not offer it to EME; PlayReady stays with the OIPF DRM agent and LG's DRM service.

Host tests: `tests/eme/run.sh` (the OpenCDM client against TVF's media server with a simulated CDM)
and `tests/eme/run-qemu.sh` (the bundle under qemu, Shaka's "Angel One" with dash.js, licenses from
`tests/eme/license.py`; `tests/eme/emecaps.html` probes which configurations WebKit grants). Under
qemu (2026-11-02) "Angel One" plays to its end through the whole chain: key system access, session,
license request and update through libocdm, WebKit's decryptor tagging, the sinks' crypto blocks,
TVF loading for DRM and feeding through the (simulated) CDM.

Two traps on the way: WebKit asks the application before it grants a key system
(`WebKitMediaKeySystemPermissionRequest`) and denies when nobody answers - tvf-web allows it
(`permission-request`); the denial still creates the CDM afterwards (the allow handler survives
`deny()`), which looks like a configuration problem. And the programs linking libWPEWebKit need
`-rpath-link` to find libocdm in stage-ocdm (`CMAKE_EXE_LINKER_FLAGS` in the media options).

## Building on a faster machine (WSL2)

The full media build took ~27 min on a PC with WSL2 + Docker (ninja 1631 s, 0 errors) against
6-7 h here. `sh wsl/pack.sh full` packs the work tree with sources and sysroot, the webOS SDK and the
build image (~850 MB), serves them in the LAN with a random token and prints one command for the build
machine; its `setup-wsl.sh` checks Docker and the location (not under `/mnt/c`), unpacks to
`~/tvf-build`, loads the image and runs `wsl-build.sh` (WebKit, install, sinks, tvf-web, bundle) which
uploads bundle and logs to `out/wsl/upload`. Later states: `sh wsl/pack.sh update` (the repo's files incl.
uncommitted changes and `stage-gst`), `update-wsl.sh` on the build machine. Changed WebKit patches reach
the already unpacked tree there through `build-webkit.sh`'s patch sync. The SDK and the TV libraries
only go to that machine.

## Zero-copy notes (later)

* Mali offers `EGL_WL_bind_wayland_display`, `EGL_KHR_image_base`, `EGL_EXT_image_dma_buf_import`,
  `GL_OES_EGL_image`, `EGL_ANDROID_native_fence_sync`.
* WPE Platform's zero-copy needs the WebProcess to render into dma-bufs: GBM (needs a DRM device) or
  `EGL_MESA_image_dma_buf_export` (Mali has none). `probe/gbmprobe.c` on the TV: the jail has `/dev/mali0`
  and `/dev/dma_buf_unified` but no `/dev/dri`; Mali's `gbm_create_device()` returns NULL for both.
* What should work: the legacy WPE API (libwpe + wpebackend-fdo, `ENABLE_WPE_LEGACY_API`), with the UI
  process inside TVF. The WebProcess renders through wayland-egl into the nested compositor that
  wpebackend-fdo runs in TVF, TVF binds its EGL display with `eglBindWaylandDisplayWL` and imports each
  frame with `eglCreateImageKHR(EGL_WAYLAND_BUFFER_WL)` as a texture - no readback, no copy. Costs: build
  libwpe/wpebackend-fdo with the SDK, embed WebKit's UI process in TVF (no separate tvf-web), lose the
  crash isolation of the helper process.
