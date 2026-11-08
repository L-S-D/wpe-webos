/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tvf-web - WPE WebKit as a separate process for TVF (native HbbTV renderer).
 *
 * WebKit runs with the WPE Platform API on a headless display and renders on
 * the CPU (Skia). Every frame is copied into one of two shared-memory buffers
 * that TVF shows as a texture over the live video; control and events are JSON
 * lines over a socket (PROTOCOL.md). A WebKit crash takes down only this
 * process.
 *
 * On the TV the whole Debian userland comes along in the bundle (lib/ with
 * Debian's ld-linux-armhf.so.3). WebKit starts its WebProcess and
 * NetworkProcess from a path compiled into libWPEWebKit
 * (/usr/lib/<triplet>/wpe-webkit-2.0/); execve()/posix_spawn() are wrapped
 * here and redirect those starts to the bundle's libexec/ through the bundled
 * loader.
 *
 *   tvf-web --fd N [--width W --height H] [--data-dir DIR]     (TVF)
 *   tvf-web --url URL --png FILE [--proxy URI] [--settle MS]   (test)
 */
#define _GNU_SOURCE
#include "cJSON.h"

#include <libsoup/soup.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#ifndef MFD_CLOEXEC /* glibc < 2.27 (webOS SDK: 2.12) - the kernel has it */
#include <sys/syscall.h>
#ifndef __NR_memfd_create
#if defined(__arm__)
#define __NR_memfd_create 385
#elif defined(__x86_64__)
#define __NR_memfd_create 319
#endif
#endif
#define MFD_CLOEXEC 1U
static int memfd_create(const char *name, unsigned flags) { return (int) syscall(__NR_memfd_create, name, flags); }
#endif

/* ------------------------------------------------------------------------ */
/* Start of the WebKit processes from the bundle                            */
/* ------------------------------------------------------------------------ */

static char g_bundle[PATH_MAX];  /* bundle root ("" = system installation) */
static char g_loader[PATH_MAX];  /* <bundle>/lib/ld-linux-*.so.* when running through it */
static char g_libdir[PATH_MAX];  /* <bundle>/lib */
static char g_libexec[PATH_MAX]; /* <bundle>/libexec */
static char g_preload[PATH_MAX]; /* <bundle>/lib/libtvfca.so */
static int g_debug;
/* tests only (qemu-user without binfmt): TVF_WEB_EXEC_PREFIX="qemu-arm -cpu cortex-a15"
 * is put in front of every redirected WebKit process */
static char g_exec_prefix_buf[512];
static char *g_exec_prefix[16];

typedef int (*execve_fn)(const char *, char *const[], char *const[]);
typedef int (*spawn_fn)(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *,
                        char *const[], char *const[]);
static execve_fn real_execve;
static spawn_fn real_posix_spawn, real_posix_spawnp;

static void resolve_real(void) {
    if (!real_execve) real_execve = (execve_fn) dlsym(RTLD_NEXT, "execve");
    if (!real_posix_spawn) real_posix_spawn = (spawn_fn) dlsym(RTLD_NEXT, "posix_spawn");
    if (!real_posix_spawnp) real_posix_spawnp = (spawn_fn) dlsym(RTLD_NEXT, "posix_spawnp");
}

/* "WPEWebProcess" etc. if path is one of WebKit's helper processes */
static const char *wk_process(const char *path) {
    const char *p = path && g_libexec[0] ? strstr(path, "/wpe-webkit-2.0/") : NULL;
    if (!p) return NULL;
    p += sizeof("/wpe-webkit-2.0/") - 1;
    return !strncmp(p, "WPE", 3) && !strchr(p, '/') ? p : NULL;
}

/* async-signal-safe (runs in the forked child of GLib's spawn) */
static void join3(char *dst, size_t n, const char *a, const char *b, const char *c) {
    size_t o = 0;
    for (const char *s = a; *s && o + 1 < n; s++) dst[o++] = *s;
    for (const char *s = b; *s && o + 1 < n; s++) dst[o++] = *s;
    for (const char *s = c; *s && o + 1 < n; s++) dst[o++] = *s;
    dst[o] = 0;
}

static void dbg_write(const char *a, const char *b) {
    if (!g_debug) return;
    ssize_t r = write(2, a, strlen(a));
    r = write(2, b, strlen(b));
    r = write(2, "\n", 1);
    (void) r;
}

/* argv for a redirected start; out[0] is the file to execute */
static void redirect_argv(const char *name, char *const argv[], char *exe, size_t exen, char **out, int max) {
    join3(exe, exen, g_libexec, "/", name);
    int n = 0;
    for (int i = 0; g_exec_prefix[i] && n < 15; i++) out[n++] = g_exec_prefix[i];
    if (g_loader[0]) {
        out[n++] = g_loader;
        out[n++] = (char *) "--library-path";
        out[n++] = g_libdir;
        if (g_preload[0]) {
            out[n++] = (char *) "--preload";
            out[n++] = g_preload;
        }
        out[n++] = (char *) "--argv0";
        out[n++] = argv && argv[0] ? argv[0] : exe;
    }
    out[n++] = exe;
    for (int i = 1; argv && argv[i] && n < max - 1; i++) out[n++] = argv[i];
    out[n] = NULL;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    resolve_real();
    const char *name = wk_process(path);
    if (!name) return real_execve(path, argv, envp);
    char exe[PATH_MAX];
    char *nargv[160];
    redirect_argv(name, argv, exe, sizeof(exe), nargv, 160);
    dbg_write("[tvf-web] execve -> ", exe);
    return real_execve(nargv[0], nargv, envp);
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
    resolve_real();
    const char *name = wk_process(path);
    if (!name) return real_posix_spawn(pid, path, fa, attr, argv, envp);
    char exe[PATH_MAX];
    char *nargv[160];
    redirect_argv(name, argv, exe, sizeof(exe), nargv, 160);
    dbg_write("[tvf-web] posix_spawn -> ", exe);
    return real_posix_spawn(pid, nargv[0], fa, attr, nargv, envp);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
    resolve_real();
    const char *name = wk_process(file);
    if (!name) return real_posix_spawnp(pid, file, fa, attr, argv, envp);
    char exe[PATH_MAX];
    char *nargv[160];
    redirect_argv(name, argv, exe, sizeof(exe), nargv, 160);
    dbg_write("[tvf-web] posix_spawnp -> ", exe);
    return real_posix_spawn(pid, nargv[0], fa, attr, nargv, envp);
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static int is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void setenv_default(const char *k, const char *v) {
    if (!getenv(k)) setenv(k, v, 1);
}

/* Bundle layout: <root>/bin/tvf-web, <root>/lib (incl. the loader),
 * <root>/libexec. Running through the loader, /proc/self/exe is the loader
 * in <root>/lib. */
static void init_bundle(const char *data_dir) {
    resolve_real();
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", exe);
    char *s = strrchr(root, '/');
    if (!s) return;
    int via_loader = !strncmp(s + 1, "ld-linux", 8);
    *s = 0; /* .../bin or .../lib */
    s = strrchr(root, '/');
    if (!s) return;
    *s = 0;
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/libexec", root);
    if (!is_dir(p)) return; /* system installation (host tests) */
    snprintf(g_bundle, sizeof(g_bundle), "%s", root);
    const char *ep = getenv("TVF_WEB_EXEC_PREFIX");
    if (ep && *ep) {
        snprintf(g_exec_prefix_buf, sizeof(g_exec_prefix_buf), "%s", ep);
        int k = 0;
        for (char *t = strtok(g_exec_prefix_buf, " "); t && k < 15; t = strtok(NULL, " ")) g_exec_prefix[k++] = t;
        g_exec_prefix[k] = NULL;
    }
    /* Debian bundle (own loader): nothing from the TV's environment may leak
     * into its userland (EGL_PLATFORM=wayland, the TV's GIO modules, GStreamer
     * plugins/registry, LD_LIBRARY_PATH). webOS-SDK bundle (TV loader, TV's
     * glibc, EGL/GLES and Wayland): keep the display environment, take our
     * libraries first - LD_LIBRARY_PATH is inherited by the WebKit processes */
    static const char *drop_debian[] = {"LD_LIBRARY_PATH", "LD_PRELOAD", "EGL_PLATFORM", "GIO_EXTRA_MODULES",
                                        "GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0", "GST_PLUGIN_SCANNER",
                                        "GST_PLUGIN_SCANNER_1_0", "GST_REGISTRY", "GST_REGISTRY_1_0",
                                        "GST_REGISTRY_UPDATE", "WAYLAND_DISPLAY", NULL};
    static const char *drop_sdk[] = {"LD_PRELOAD", "GIO_EXTRA_MODULES", "GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0",
                                     "GST_PLUGIN_SCANNER", "GST_PLUGIN_SCANNER_1_0", "GST_REGISTRY",
                                     "GST_REGISTRY_1_0", "GST_REGISTRY_UPDATE", NULL};
    const char **drop = via_loader ? drop_debian : drop_sdk;
    for (int i = 0; drop[i]; i++) unsetenv(drop[i]);
    snprintf(g_libexec, sizeof(g_libexec), "%s", p);
    snprintf(g_libdir, sizeof(g_libdir), "%s/lib", root);
    if (via_loader) snprintf(g_loader, sizeof(g_loader), "%s", exe);
    else setenv("LD_LIBRARY_PATH", g_libdir, 1);
    snprintf(p, sizeof(p), "%s/lib/libtvfca.so", root);
    if (is_file(p)) snprintf(g_preload, sizeof(g_preload), "%s", p);

    snprintf(p, sizeof(p), "%s/lib/gio/modules", root);
    setenv("GIO_MODULE_DIR", p, 1);
    snprintf(p, sizeof(p), "%s/libexec/injected-bundle", root);
    setenv_default("WEBKIT_INJECTED_BUNDLE_PATH", p);
    snprintf(p, sizeof(p), "%s/etc/fonts/fonts.conf", root);
    setenv_default("FONTCONFIG_FILE", p);
    snprintf(p, sizeof(p), "%s/etc/ssl/ca-certificates.crt", root);
    setenv_default("TVF_WEB_CA_FILE", p);
    setenv_default("SSL_CERT_FILE", p); /* glib-networking with OpenSSL (SDK bundle) */
    snprintf(p, sizeof(p), "%s/lib/gstreamer-1.0", root);
    setenv_default("GST_PLUGIN_SYSTEM_PATH", p);
    /* EGL: Mesa (surfaceless, llvmpipe) from the bundle through glvnd -
     * WebKit's WebProcess needs an EGL display even for CPU rendering */
    snprintf(p, sizeof(p), "%s/share/glvnd/egl_vendor.d/50_mesa.json", root);
    if (is_file(p)) setenv_default("__EGL_VENDOR_LIBRARY_FILENAMES", p);
    snprintf(p, sizeof(p), "%s/share/X11/xkb", root);
    setenv_default("XKB_CONFIG_ROOT", p);
    if (data_dir) {
        snprintf(p, sizeof(p), "%s/mesa-cache", data_dir);
        setenv_default("MESA_SHADER_CACHE_DIR", p);
        snprintf(p, sizeof(p), "%s/gst-registry.bin", data_dir);
        setenv_default("GST_REGISTRY", p);
    } else
        setenv_default("MESA_SHADER_CACHE_DISABLE", "true");
}

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

static GMainLoop *g_loop;
static WebKitWebView *g_view;
static WPEView *g_wview;
static WebKitNetworkSession *g_session;
static SoupSession *g_soup; /* own fetches (HbbTV pages with their MIME type) */
static char g_proxy_uri[512];
static int g_w = 1280, g_h = 720;
static double g_t0;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* control mode */
static int g_ctl = -1;
static int g_memfd = -1;
static uint8_t *g_shm;
static size_t g_bufsize; /* one buffer: W*H*4 */
static int g_held[2];    /* frame sent, no ack yet */
static uint32_t g_sent[2];
static int g_last = -1; /* buffer of the last sent frame */
static uint32_t g_seq;
static uint8_t *g_pending; /* newest frame while both buffers are held */
static int g_has_pending;
static GString *g_inbuf;

/* test mode */
static const char *t_url, *t_png, *t_js, *t_keys, *t_script;
static int t_settle = 3000, t_timeout = 30000;
static uint32_t t_bg = 0x808080;
static uint8_t *t_frame, *t_frame_max;
static long t_max_vis = -1;
static double t_max_at;
static int t_frames;
static double t_first_frame, t_loaded;
static guint t_done_src;
static double t_keys_at, t_keys_vis;

/* ------------------------------------------------------------------------ */
/* Messages to TVF                                                          */
/* ------------------------------------------------------------------------ */

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t) w;
    }
    return 0;
}

/* JSON line; with fd >= 0 the descriptor travels along (SCM_RIGHTS) */
static void send_json(cJSON *j, int fd) {
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return;
    if (g_ctl < 0) {
        printf("%s\n", s);
        fflush(stdout);
        free(s);
        return;
    }
    size_t n = strlen(s);
    s[n] = '\n'; /* replaces the terminator, length n + 1 */
    if (fd >= 0) {
        struct iovec iov = {s, n + 1};
        char cbuf[CMSG_SPACE(sizeof(int))];
        memset(cbuf, 0, sizeof(cbuf));
        struct msghdr mh = {0};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
        ssize_t w;
        do w = sendmsg(g_ctl, &mh, MSG_NOSIGNAL);
        while (w < 0 && errno == EINTR);
        if (w >= 0 && (size_t) w < n + 1) write_all(g_ctl, s + w, n + 1 - (size_t) w);
    } else
        write_all(g_ctl, s, n + 1);
    free(s);
}

static cJSON *ev(const char *name) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "event", name);
    return j;
}

/* ------------------------------------------------------------------------ */
/* Frames                                                                   */
/* ------------------------------------------------------------------------ */

static int alloc_buffers(void) {
    if (g_shm) munmap(g_shm, 2 * g_bufsize);
    if (g_memfd >= 0) close(g_memfd);
    g_shm = NULL;
    g_bufsize = (size_t) g_w * g_h * 4;
    g_memfd = memfd_create("tvf-web-frames", MFD_CLOEXEC);
    if (g_memfd < 0 || ftruncate(g_memfd, (off_t) (2 * g_bufsize)) < 0) return -1;
    g_shm = mmap(NULL, 2 * g_bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, g_memfd, 0);
    if (g_shm == MAP_FAILED) {
        g_shm = NULL;
        return -1;
    }
    memset(g_shm, 0, 2 * g_bufsize);
    g_held[0] = g_held[1] = 0;
    g_last = -1;
    g_has_pending = 0;
    free(g_pending);
    g_pending = malloc(g_bufsize);
    return g_pending ? 0 : -1;
}

static void send_buffers(const char *name) {
    cJSON *j = ev(name);
    cJSON_AddNumberToObject(j, "width", g_w);
    cJSON_AddNumberToObject(j, "height", g_h);
    cJSON_AddNumberToObject(j, "stride", g_w * 4);
    cJSON_AddStringToObject(j, "format", "BGRA8-premultiplied");
    cJSON_AddNumberToObject(j, "buffers", 2);
    cJSON_AddNumberToObject(j, "bufferSize", (double) g_bufsize);
    cJSON_AddNumberToObject(j, "pid", getpid());
    send_json(j, g_memfd);
}

/* bounding box of the pixels that differ between a and b; 0 = identical */
static int diff_box(const uint8_t *a, const uint8_t *b, int *box) {
    int y0 = -1, y1 = -1, x0 = g_w, x1 = -1;
    size_t row = (size_t) g_w * 4;
    for (int y = 0; y < g_h; y++) {
        const uint32_t *pa = (const uint32_t *) (a + y * row), *pb = (const uint32_t *) (b + y * row);
        if (!memcmp(pa, pb, row)) continue;
        if (y0 < 0) y0 = y;
        y1 = y;
        int l = 0, r = g_w - 1;
        while (l < x0 && pa[l] == pb[l]) l++;
        while (r > x1 && pa[r] == pb[r]) r--;
        if (l < x0) x0 = l;
        if (r > x1) x1 = r;
    }
    if (y0 < 0) return 0;
    box[0] = x0;
    box[1] = y0;
    box[2] = x1 - x0 + 1;
    box[3] = y1 - y0 + 1;
    return 1;
}

/* frame (W*H*4, tightly packed) into the free buffer and announce it */
static void push_frame(const uint8_t *px) {
    int b = g_last == 0 ? 1 : 0;
    if (g_held[b]) {
        if (g_last >= 0 && g_held[g_last] && g_held[b]) {
            memcpy(g_pending, px, g_bufsize);
            g_has_pending = 1;
            return;
        }
    }
    uint8_t *dst = g_shm + b * g_bufsize;
    int box[4] = {0, 0, g_w, g_h};
    int changed = 1;
    if (g_last >= 0) changed = diff_box(px, g_shm + g_last * g_bufsize, box);
    if (!changed) return;
    memcpy(dst, px, g_bufsize);
    g_held[b] = 1;
    g_sent[b] = ++g_seq;
    g_last = b;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "frame", g_seq);
    cJSON_AddNumberToObject(j, "buf", b);
    cJSON *d = cJSON_AddArrayToObject(j, "damage");
    for (int i = 0; i < 4; i++) cJSON_AddItemToArray(d, cJSON_CreateNumber(box[i]));
    send_json(j, -1);
}

static void on_ack(uint32_t seq) {
    for (int b = 0; b < 2; b++)
        if (g_held[b] && g_sent[b] == seq) g_held[b] = 0;
    if (g_has_pending && (!g_held[0] || !g_held[1])) {
        g_has_pending = 0;
        push_frame(g_pending);
    }
}

/* pixels of a rendered buffer, tightly packed W*H*4 (BGRA premultiplied) */
static const uint8_t *buffer_pixels(WPEBuffer *buf, uint8_t *tmp, GBytes **keep) {
    int w = wpe_buffer_get_width(buf), h = wpe_buffer_get_height(buf);
    if (w != g_w || h != g_h) return NULL;
    const uint8_t *data;
    gsize len = 0;
    guint stride = (guint) w * 4;
    if (WPE_IS_BUFFER_SHM(buf)) {
        GBytes *bytes = wpe_buffer_shm_get_data(WPE_BUFFER_SHM(buf));
        data = g_bytes_get_data(bytes, &len);
        stride = wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buf));
    } else {
        GError *err = NULL;
        *keep = wpe_buffer_import_to_pixels(buf, &err);
        if (!*keep) {
            fprintf(stderr, "[tvf-web] import_to_pixels: %s\n", err ? err->message : "?");
            g_clear_error(&err);
            return NULL;
        }
        data = g_bytes_get_data(*keep, &len);
    }
    if (!data || len < (gsize) stride * (h - 1) + (gsize) w * 4) return NULL;
    if (stride == (guint) w * 4) return data;
    for (int y = 0; y < h; y++) memcpy(tmp + (size_t) y * w * 4, data + (size_t) y * stride, (size_t) w * 4);
    return tmp;
}

static void on_buffer_rendered(WPEView *view, WPEBuffer *buf, gpointer user) {
    (void) view;
    (void) user;
    static uint8_t *tmp;
    static size_t tmpn;
    size_t need = (size_t) g_w * g_h * 4;
    if (tmpn != need) {
        free(tmp);
        tmp = malloc(need);
        tmpn = tmp ? need : 0;
    }
    if (!tmp) return;
    GBytes *keep = NULL;
    const uint8_t *px = buffer_pixels(buf, tmp, &keep);
    if (px) {
        if (!t_frames++) {
            t_first_frame = now_ms() - g_t0;
            if (g_debug)
                fprintf(stderr, "[tvf-web] erstes Bild nach %.0f ms (%s)\n", t_first_frame,
                        WPE_IS_BUFFER_SHM(buf) ? "SHM" : "DMA-BUF/sonstiges");
        }
        if (g_debug > 1) {
            long op = 0;
            for (size_t i = 3; i < need; i += 64) op += px[i] != 0;
            fprintf(stderr, "[tvf-web] Bild %d nach %.0f ms, ~%ld%% sichtbar\n", t_frames, now_ms() - g_t0,
                    op * 100 / (long) (need / 64));
        }
        if (g_ctl >= 0 && g_shm) push_frame(px);
        if (t_png && t_frame) {
            memcpy(t_frame, px, need);
            long vis = 0;
            for (size_t i = 3; i < need; i += 64) vis += px[i] != 0;
            if (t_keys_at > 0 && !t_keys_vis && vis * 2 > (long) (need / 64)) {
                t_keys_vis = now_ms() - g_t0 - t_keys_at;
                printf("Taste -> mehr als halb sichtbar nach %.0f ms\n", t_keys_vis);
            }
            if (vis > t_max_vis && t_frame_max) {
                t_max_vis = vis;
                t_max_at = now_ms() - g_t0;
                memcpy(t_frame_max, px, need);
            }
        }
    }
    if (keep) g_bytes_unref(keep);
}

/* ------------------------------------------------------------------------ */
/* PNG (test mode)                                                          */
/* ------------------------------------------------------------------------ */

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t len[4] = {(uint8_t) (n >> 24), (uint8_t) (n >> 16), (uint8_t) (n >> 8), (uint8_t) n};
    fwrite(len, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    uLong crc = crc32(0, (const Bytef *) type, 4);
    if (n) crc = crc32(crc, data, n);
    uint8_t c[4] = {(uint8_t) (crc >> 24), (uint8_t) (crc >> 16), (uint8_t) (crc >> 8), (uint8_t) crc};
    fwrite(c, 1, 4, f);
}

/* premultiplied BGRA over a solid background (bg = 0xRRGGBB) as RGB PNG;
 * returns the number of non-transparent pixels */
static long write_png(const char *path, const uint8_t *px, int w, int h, uint32_t bg) {
    size_t row = (size_t) w * 3 + 1;
    uint8_t *raw = malloc(row * h);
    long opaque = 0;
    int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    for (int y = 0; y < h; y++) {
        uint8_t *o = raw + y * row;
        *o++ = 0;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t) y * w + x) * 4;
            int a = p[3];
            if (a) opaque++;
            /* premultiplied: out = src + bg * (1 - a) */
            o[0] = (uint8_t) (p[2] + br * (255 - a) / 255);
            o[1] = (uint8_t) (p[1] + bgc * (255 - a) / 255);
            o[2] = (uint8_t) (p[0] + bb * (255 - a) / 255);
            o += 3;
        }
    }
    uLongf zl = compressBound(row * h);
    uint8_t *z = malloc(zl);
    compress2(z, &zl, raw, row * h, 6);
    FILE *f = fopen(path, "wb");
    if (f) {
        static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        fwrite(sig, 1, 8, f);
        uint8_t ihdr[13] = {(uint8_t) (w >> 24), (uint8_t) (w >> 16), (uint8_t) (w >> 8), (uint8_t) w,
                            (uint8_t) (h >> 24), (uint8_t) (h >> 16), (uint8_t) (h >> 8), (uint8_t) h,
                            8, 2, 0, 0, 0};
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (uint32_t) zl);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw);
    free(z);
    return opaque;
}

/* PSS and private dirty memory (kB) from smaps_rollup */
static void pss_kb(pid_t pid, long *pss, long *priv) {
    char p[64], line[256];
    snprintf(p, sizeof(p), "/proc/%d/smaps_rollup", (int) pid);
    FILE *f = fopen(p, "r");
    *pss = *priv = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "Pss:", 4)) *pss = atol(line + 4);
        else if (!strncmp(line, "Private_Dirty:", 14)) *priv = atol(line + 14);
    }
    fclose(f);
}

/* RSS (kB) of this process and its descendants */
static long rss_kb(pid_t pid) {
    char p[64], line[256];
    snprintf(p, sizeof(p), "/proc/%d/status", (int) pid);
    FILE *f = fopen(p, "r");
    long kb = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "VmRSS:", 6)) kb = atol(line + 6);
    fclose(f);
    return kb;
}

static void print_rss(void) {
    pid_t me = getpid();
    long total = rss_kb(me), pss, priv, tpss, tpriv;
    pss_kb(me, &tpss, &tpriv);
    printf("RSS tvf-web %ld kB (PSS %ld, privat %ld)\n", total, tpss, tpriv);
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        char p[64], line[512];
        snprintf(p, sizeof(p), "/proc/%d/status", pid);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int ppid = -1;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "PPid:", 5)) ppid = atoi(line + 5);
        fclose(f);
        if (ppid != me) continue;
        char cmd[512] = "";
        snprintf(p, sizeof(p), "/proc/%d/cmdline", pid);
        f = fopen(p, "r");
        if (f) {
            size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
            fclose(f);
            cmd[n] = 0;
            /* through the loader: the program is the argument after "--argv0 X" */
            const char *name = cmd;
            for (size_t i = 0; i < n; i += strlen(cmd + i) + 1)
                if (strstr(cmd + i, "/WPE")) name = strrchr(cmd + i, '/') + 1;
            long kb = rss_kb(pid);
            pss_kb(pid, &pss, &priv);
            total += kb;
            tpss += pss;
            tpriv += priv;
            printf("RSS %s (pid %d) %ld kB (PSS %ld, privat %ld)\n", name, pid, kb, pss, priv);
        }
    }
    if (d) closedir(d);
    printf("RSS gesamt %ld kB, PSS %ld kB, privat %ld kB\n", total, tpss, tpriv);
}

/* ------------------------------------------------------------------------ */
/* Keys                                                                     */
/* ------------------------------------------------------------------------ */

/* DOM keyCode -> WPE keyval for keys WebKit maps itself */
static guint keyval_for(int code) {
    switch (code) {
    case 37: return WPE_KEY_Left;
    case 38: return WPE_KEY_Up;
    case 39: return WPE_KEY_Right;
    case 40: return WPE_KEY_Down;
    case 13: return WPE_KEY_Return;
    case 8: return WPE_KEY_BackSpace;
    case 27: return WPE_KEY_Escape;
    case 32: return WPE_KEY_space;
    case 33: return WPE_KEY_Page_Up;
    case 34: return WPE_KEY_Page_Down;
    /* WebKit maps the colour keysyms to 403-406 (key "ColorF0Red" ...) */
    case 403: return WPE_KEY_Red;
    case 404: return WPE_KEY_Green;
    case 405: return WPE_KEY_Yellow;
    case 406: return WPE_KEY_Blue;
    default:
        if (code >= 48 && code <= 57) return WPE_KEY_0 + (guint) (code - 48);
        return 0;
    }
}

static int g_native_all; /* test: also send the other codes as native events */

static guint keyval_extra(int code) {
    switch (code) {
    case 403: return WPE_KEY_Red;
    case 404: return WPE_KEY_Green;
    case 405: return WPE_KEY_Yellow;
    case 406: return WPE_KEY_Blue;
    case 461: return WPE_KEY_Back;
    case 415: return WPE_KEY_AudioPlay;
    case 19: return WPE_KEY_AudioPause;
    case 413: return WPE_KEY_AudioStop;
    case 417: return WPE_KEY_AudioForward;
    case 412: return WPE_KEY_AudioRewind;
    default: return 0;
    }
}

static void send_key(int code, int down) {
    guint kv = keyval_for(code);
    if (!kv && g_native_all) kv = keyval_extra(code);
    if (kv) {
        WPEEvent *e = wpe_event_keyboard_new(down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, g_wview,
                                             WPE_INPUT_SOURCE_KEYBOARD, (guint32) now_ms(), 0, 0, kv);
        wpe_view_event(g_wview, e);
        wpe_event_unref(e);
        return;
    }
    /* VK_BACK 461 and the media keys (415, 19, 413, 417, 412): WebKit gives
     * them other keyCodes (BrowserBack 166, MediaPlay 250) - synthetic event
     * in the page with the HbbTV code (isTrusted false) */
    char js[700];
    snprintf(js, sizeof(js),
             "(function(c,t){var e;try{e=new KeyboardEvent(t,{bubbles:true,cancelable:true,keyCode:c,which:c});}"
             "catch(x){e=document.createEvent('Event');e.initEvent(t,true,true);}"
             "if(e.keyCode!==c){try{Object.defineProperty(e,'keyCode',{get:function(){return c}});"
             "Object.defineProperty(e,'which',{get:function(){return c}});}catch(x){}}"
             "(document.activeElement||document.body||document).dispatchEvent(e);})(%d,'%s')",
             code, down ? "keydown" : "keyup");
    webkit_web_view_evaluate_javascript(g_view, js, -1, NULL, NULL, NULL, NULL, NULL);
}

/* Pointer (page coordinates) from TVF: the Magic Remote pointer, only for
 * pages that set keyset.supportsPointer. One button (primary); like the
 * Wayland seat the modifiers already carry the button on press */
static guint32 g_buttons;

static void send_pointer(const char *type, double x, double y) {
    guint32 t = (guint32) now_ms();
    WPEEvent *e = NULL;
    if (!strcmp(type, "move")) {
        e = wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, g_wview, WPE_INPUT_SOURCE_MOUSE, t, g_buttons, x, y, 0, 0);
    } else if (!strcmp(type, "down") || !strcmp(type, "up")) {
        int down = type[0] == 'd';
        g_buttons = down ? WPE_MODIFIER_POINTER_BUTTON1 : 0;
        guint n = down ? wpe_view_compute_press_count(g_wview, x, y, WPE_BUTTON_PRIMARY, t) : 0;
        e = wpe_event_pointer_button_new(down ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP, g_wview,
                                         WPE_INPUT_SOURCE_MOUSE, t, g_buttons, WPE_BUTTON_PRIMARY, x, y, n);
    } else if (!strcmp(type, "leave")) {
        g_buttons = 0;
        e = wpe_event_pointer_move_new(WPE_EVENT_POINTER_LEAVE, g_wview, WPE_INPUT_SOURCE_MOUSE, t, 0, -1, -1, 0, 0);
    }
    if (!e) return;
    wpe_view_event(g_wview, e);
    wpe_event_unref(e);
}

/* Wheel steps at (x, y): dy > 0 = up (away from the user), like WPE's
 * Wayland seat (it negates the Wayland axis value) */
static void send_scroll(double dy, double x, double y) {
    WPEEvent *e = wpe_event_scroll_new(g_wview, WPE_INPUT_SOURCE_MOUSE, (guint32) now_ms(), g_buttons, 0, dy, FALSE,
                                       FALSE, x, y);
    wpe_view_event(g_wview, e);
    wpe_event_unref(e);
}

/* ------------------------------------------------------------------------ */
/* WebKit                                                                   */
/* ------------------------------------------------------------------------ */

static void set_proxy(const char *uri) {
    snprintf(g_proxy_uri, sizeof(g_proxy_uri), "%s", uri ? uri : "");
    if (g_soup) {
        GProxyResolver *r = g_proxy_uri[0] ? g_simple_proxy_resolver_new(g_proxy_uri, NULL) : NULL;
        soup_session_set_proxy_resolver(g_soup, r);
        if (r) g_object_unref(r);
    }
    if (uri && *uri) {
        WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(uri, NULL);
        webkit_network_session_set_proxy_settings(g_session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
        webkit_network_proxy_settings_free(ps);
    } else
        webkit_network_session_set_proxy_settings(g_session, WEBKIT_NETWORK_PROXY_MODE_NO_PROXY, NULL);
}

static char *js_to_text(JSCValue *v) {
    if (!v) return g_strdup("");
    if (jsc_value_is_string(v)) return jsc_value_to_string(v);
    char *s = jsc_value_to_json(v, 0);
    return s ? s : jsc_value_to_string(v);
}

static void on_script_msg(WebKitUserContentManager *m, JSCValue *v, gpointer user) {
    (void) m;
    const char *kind = user;
    char *s = js_to_text(v);
    if (!strcmp(kind, "console")) {
        cJSON *j = ev("console");
        cJSON_AddStringToObject(j, "text", s);
        if (g_ctl >= 0) send_json(j, -1);
        else {
            fprintf(stderr, "[console] %s\n", s);
            cJSON_Delete(j);
        }
    } else {
        cJSON *j = ev("msg");
        cJSON *data = cJSON_Parse(s);
        if (data) cJSON_AddItemToObject(j, "data", data);
        else cJSON_AddStringToObject(j, "data", s);
        if (g_ctl >= 0) send_json(j, -1);
        else {
            fprintf(stderr, "[msg] %s\n", s);
            cJSON_Delete(j);
        }
    }
    g_free(s);
}

static gboolean test_keys(gpointer user);

static gboolean test_finish(gpointer user) {
    (void) user;
    t_done_src = 0;
    if (t_png && t_frame) {
        long opaque = write_png(t_png, t_frame, g_w, g_h, t_bg);
        printf("PNG %s: %d Bilder, %ld von %d Pixeln nicht transparent\n", t_png, t_frames, opaque, g_w * g_h);
        if (t_frame_max && t_max_vis > 0) {
            char mp[PATH_MAX];
            snprintf(mp, sizeof(mp), "%.*s-max.png", (int) (strlen(t_png) > 4 ? strlen(t_png) - 4 : strlen(t_png)), t_png);
            long o2 = write_png(mp, t_frame_max, g_w, g_h, t_bg);
            printf("PNG %s: sichtbarstes Bild nach %.0f ms, %ld Pixel nicht transparent\n", mp, t_max_at, o2);
        }
    }
    printf("erstes Bild %.0f ms, geladen %.0f ms\n", t_first_frame, t_loaded);
    print_rss();
    fflush(stdout);
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

static void on_eval_done(GObject *o, GAsyncResult *r, gpointer user) {
    GError *err = NULL;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(o), r, &err);
    int id = GPOINTER_TO_INT(user);
    char *s = v ? js_to_text(v) : NULL;
    if (g_ctl >= 0) {
        cJSON *j = ev("evalResult");
        cJSON_AddNumberToObject(j, "id", id);
        if (err) cJSON_AddStringToObject(j, "error", err->message);
        else {
            cJSON *val = cJSON_Parse(s ? s : "null");
            if (val) cJSON_AddItemToObject(j, "value", val);
            else cJSON_AddStringToObject(j, "value", s ? s : "");
        }
        send_json(j, -1);
    } else
        printf("JS: %s\n", err ? err->message : s);
    g_free(s);
    if (v) g_object_unref(v);
    g_clear_error(&err);
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent e, gpointer user) {
    (void) user;
    if (e != WEBKIT_LOAD_FINISHED) return;
    t_loaded = now_ms() - g_t0;
    if (g_debug) fprintf(stderr, "[tvf-web] geladen nach %.0f ms: %s\n", t_loaded, webkit_web_view_get_uri(v));
    if (g_ctl >= 0) {
        cJSON *j = ev("loaded");
        cJSON_AddStringToObject(j, "url", webkit_web_view_get_uri(v));
        send_json(j, -1);
    } else {
        if (t_js) webkit_web_view_evaluate_javascript(v, t_js, -1, NULL, NULL, NULL, on_eval_done, NULL);
        if (t_keys) g_timeout_add(t_settle / 2, test_keys, (gpointer) t_keys);
        if (!t_done_src || t_loaded > 0) {
            if (t_done_src) g_source_remove(t_done_src);
            t_done_src = g_timeout_add(t_settle, test_finish, NULL);
        }
    }
}

static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent e, char *uri, GError *err, gpointer user) {
    (void) v;
    (void) e;
    (void) user;
    cJSON *j = ev("loadFailed");
    cJSON_AddStringToObject(j, "url", uri);
    cJSON_AddStringToObject(j, "error", err ? err->message : "");
    if (g_ctl >= 0) send_json(j, -1);
    else {
        char *s = cJSON_PrintUnformatted(j);
        printf("%s\n", s);
        free(s);
        cJSON_Delete(j);
    }
    return FALSE;
}

static gboolean on_tls_error(WebKitWebView *v, char *uri, GTlsCertificate *cert, GTlsCertificateFlags flags,
                             gpointer user) {
    (void) v;
    (void) cert;
    (void) user;
    cJSON *j = ev("tlsError");
    cJSON_AddStringToObject(j, "url", uri);
    cJSON_AddNumberToObject(j, "flags", flags);
    if (g_ctl >= 0) send_json(j, -1);
    else {
        char *s = cJSON_PrintUnformatted(j);
        printf("%s\n", s);
        free(s);
        cJSON_Delete(j);
    }
    return FALSE; /* keep WebKit's error page - nothing is accepted */
}

/* EME: WebKit asks the application before it gives a page a key system
 * (navigator.requestMediaKeySystemAccess); unanswered means denied. The key
 * systems are the TV's, through TVF (libocdm) - allowed. Everything else keeps
 * WebKit's default (deny). */
static gboolean on_permission_request(WebKitWebView *v, WebKitPermissionRequest *r, gpointer user) {
    (void) v;
    (void) user;
    if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(r)) {
        fprintf(stderr, "[web] EME: %s erlaubt\n",
                webkit_media_key_system_permission_get_name(WEBKIT_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(r)));
        webkit_permission_request_allow(r);
        return TRUE;
    }
    return FALSE;
}

static void on_terminated(WebKitWebView *v, WebKitWebProcessTerminationReason r, gpointer user) {
    (void) v;
    (void) user;
    cJSON *j = ev("crashed");
    cJSON_AddStringToObject(j, "reason", r == WEBKIT_WEB_PROCESS_CRASHED                ? "crashed"
                                         : r == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "memory"
                                                                                          : "terminated");
    if (g_ctl >= 0) send_json(j, -1);
    else {
        char *s = cJSON_PrintUnformatted(j);
        printf("%s\n", s);
        free(s);
        cJSON_Delete(j);
        if (!t_done_src) t_done_src = g_timeout_add(10, test_finish, NULL);
    }
}

/* HbbTV documents come as application/vnd.hbbtv.xhtml+xml (or CE-HTML),
 * which WebKit does not display ("Frame load interrupted"). For the main
 * document: refuse WebKit's response, fetch the page again and load it as
 * text/html with its real address as base - origin, cookies set later and
 * relative addresses stay those of the server (like the gateway's forward
 * proxy, which delivers HbbTV XHTML as text/html, too). */
static int hbbtv_mime(const char *m) {
    return m && (strstr(m, "vnd.hbbtv") || strstr(m, "ce-html") || strstr(m, "vnd.oipf"));
}

static void on_refetched(GObject *src, GAsyncResult *res, gpointer user) {
    char *uri = user;
    GError *err = NULL;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (body) {
        if (g_debug) fprintf(stderr, "[tvf-web] HbbTV-Seite als text/html: %s (%zu Bytes)\n", uri, g_bytes_get_size(body));
        webkit_web_view_load_bytes(g_view, body, "text/html", NULL, uri);
        g_bytes_unref(body);
    } else {
        cJSON *j = ev("loadFailed");
        cJSON_AddStringToObject(j, "url", uri);
        cJSON_AddStringToObject(j, "error", err ? err->message : "refetch");
        if (g_ctl >= 0) send_json(j, -1);
        else {
            char *t = cJSON_PrintUnformatted(j);
            printf("%s\n", t);
            free(t);
            cJSON_Delete(j);
        }
    }
    g_clear_error(&err);
    g_free(uri);
}

static gboolean on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType type,
                                 gpointer user) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE) return FALSE;
    WebKitResponsePolicyDecision *rd = WEBKIT_RESPONSE_POLICY_DECISION(d);
    if (!webkit_response_policy_decision_is_main_frame_main_resource(rd) ||
        webkit_response_policy_decision_is_mime_type_supported(rd))
        return FALSE;
    WebKitURIResponse *resp = webkit_response_policy_decision_get_response(rd);
    const char *mime = webkit_uri_response_get_mime_type(resp);
    if (!hbbtv_mime(mime)) return FALSE;
    const char *uri = webkit_uri_response_get_uri(resp);
    webkit_policy_decision_ignore(d);
    SoupMessage *m = soup_message_new("GET", uri);
    if (!m) return TRUE;
    SoupMessageHeaders *h = soup_message_get_request_headers(m);
    const char *ua = webkit_settings_get_user_agent(webkit_web_view_get_settings(v));
    if (ua) soup_message_headers_replace(h, "User-Agent", ua);
    soup_message_headers_replace(h, "Accept", "application/vnd.hbbtv.xhtml+xml,application/xhtml+xml,text/html,*/*");
    soup_session_send_and_read_async(g_soup, m, G_PRIORITY_DEFAULT, NULL, on_refetched, g_strdup(uri));
    g_object_unref(m);
    return TRUE;
}

static void add_user_script(const char *js) {
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(g_view);
    WebKitUserScript *us = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                  WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, us);
    webkit_user_script_unref(us);
}

static const char *CONSOLE_HOOK =
    "(function(){var h=window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.tvfconsole;"
    "if(!h)return;function s(a){try{return typeof a==='string'?a:JSON.stringify(a)}catch(e){return String(a)}}"
    "['log','info','warn','error','debug'].forEach(function(l){var o=console[l];console[l]=function(){"
    "try{h.postMessage(l+' '+Array.prototype.map.call(arguments,s).join(' '))}catch(e){}"
    "return o.apply(console,arguments)}});"
    "window.addEventListener('error',function(e){try{h.postMessage('error '+e.message+' @'+e.filename+':'+e.lineno)}"
    "catch(x){}});})();";

static int create_view(const char *data_dir, const char *ua, int media) {
    WPEDisplay *display = wpe_display_headless_new();
    GError *err = NULL;
    if (!wpe_display_connect(display, &err)) {
        fprintf(stderr, "[tvf-web] wpe_display_connect: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    }
    if (data_dir && *data_dir) {
        char *d = g_build_filename(data_dir, "data", NULL), *c = g_build_filename(data_dir, "cache", NULL);
        g_session = webkit_network_session_new(d, c);
        g_free(d);
        g_free(c);
    } else
        g_session = webkit_network_session_new_ephemeral();
    webkit_network_session_set_tls_errors_policy(g_session, WEBKIT_TLS_ERRORS_POLICY_FAIL);
    g_soup = soup_session_new_with_options("timeout", 20, NULL);
    const char *ca = getenv("TVF_WEB_CA_FILE");
    if (ca && is_file(ca)) {
        GTlsDatabase *db = g_tls_file_database_new(ca, NULL);
        if (db) {
            soup_session_set_tls_database(g_soup, db);
            g_object_unref(db);
        }
    }
    WebKitWebContext *ctx = webkit_web_context_new();
    webkit_web_context_set_cache_model(ctx, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

    WebKitSettings *s = webkit_settings_new();
    if (ua && *ua) webkit_settings_set_user_agent(s, ua);
    webkit_settings_set_enable_media(s, media);
    webkit_settings_set_enable_webaudio(s, media);
    webkit_settings_set_enable_mediasource(s, media);
    /* EME (needs a build with ENABLE_ENCRYPTED_MEDIA: ClearKey for now) */
    webkit_settings_set_enable_encrypted_media(s, media);
    webkit_settings_set_enable_media_stream(s, FALSE);
    webkit_settings_set_enable_page_cache(s, FALSE);
    webkit_settings_set_enable_developer_extras(s, TRUE);
    webkit_settings_set_enable_2d_canvas_acceleration(s, FALSE);

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "tvf", NULL);
    webkit_user_content_manager_register_script_message_handler(ucm, "tvfconsole", NULL);
    g_signal_connect(ucm, "script-message-received::tvf", G_CALLBACK(on_script_msg), (gpointer) "msg");
    g_signal_connect(ucm, "script-message-received::tvfconsole", G_CALLBACK(on_script_msg), (gpointer) "console");
    WebKitUserScript *us = webkit_user_script_new(CONSOLE_HOOK, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                  WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, us);
    webkit_user_script_unref(us);

    /* a TV page starts media with sound without a click (HbbTV), WebKit's
     * default would pause an unmuted autoplay */
    WebKitWebsitePolicies *wp = webkit_website_policies_new_with_policies("autoplay", WEBKIT_AUTOPLAY_ALLOW, NULL);
    g_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "network-session", g_session,
                                          "web-context", ctx, "settings", s, "user-content-manager", ucm,
                                          "website-policies", wp, NULL));
    g_object_unref(wp);
    if (!g_view) return -1;
    WebKitColor transparent = {0, 0, 0, 0};
    webkit_web_view_set_background_color(g_view, &transparent);
    g_wview = webkit_web_view_get_wpe_view(g_view);
    WPEToplevel *tl = wpe_view_get_toplevel(g_wview);
    if (!tl) {
        tl = wpe_display_create_toplevel(display, 1);
        wpe_view_set_toplevel(g_wview, tl);
    }
    if (tl && !wpe_toplevel_resize(tl, g_w, g_h)) wpe_toplevel_resized(tl, g_w, g_h);
    wpe_view_map(g_wview);
    wpe_view_set_visible(g_wview, TRUE);
    wpe_view_focus_in(g_wview);
    if (g_debug)
        fprintf(stderr, "[tvf-web] Ansicht %dx%d, toplevel %p\n", wpe_view_get_width(g_wview),
                wpe_view_get_height(g_wview), (void *) tl);
    g_signal_connect(g_wview, "buffer-rendered", G_CALLBACK(on_buffer_rendered), NULL);
    g_signal_connect(g_view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(g_view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(g_view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(g_view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), NULL);
    g_signal_connect(g_view, "web-process-terminated", G_CALLBACK(on_terminated), NULL);
    g_signal_connect(g_view, "permission-request", G_CALLBACK(on_permission_request), NULL);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Control socket                                                           */
/* ------------------------------------------------------------------------ */

static void handle_cmd(cJSON *j) {
    const cJSON *ack = cJSON_GetObjectItemCaseSensitive(j, "ack");
    if (cJSON_IsNumber(ack)) {
        on_ack((uint32_t) ack->valuedouble);
        return;
    }
    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "cmd"));
    if (!cmd) return;
    if (!strcmp(cmd, "load")) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "url"));
        const char *ua = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "userAgent"));
        const cJSON *px = cJSON_GetObjectItemCaseSensitive(j, "proxy");
        if (ua) webkit_settings_set_user_agent(webkit_web_view_get_settings(g_view), ua);
        if (px) set_proxy(cJSON_GetStringValue(px));
        if (url) webkit_web_view_load_uri(g_view, url);
    } else if (!strcmp(cmd, "key")) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "code");
        if (cJSON_IsNumber(c)) send_key((int) c->valuedouble, !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "down")));
    } else if (!strcmp(cmd, "pointer")) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "type"));
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(j, "x"), *y = cJSON_GetObjectItemCaseSensitive(j, "y");
        if (t) send_pointer(t, cJSON_IsNumber(x) ? x->valuedouble : -1, cJSON_IsNumber(y) ? y->valuedouble : -1);
    } else if (!strcmp(cmd, "scroll")) {
        const cJSON *dy = cJSON_GetObjectItemCaseSensitive(j, "dy");
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(j, "x"), *y = cJSON_GetObjectItemCaseSensitive(j, "y");
        if (cJSON_IsNumber(dy) && dy->valuedouble != 0)
            send_scroll(dy->valuedouble, cJSON_IsNumber(x) ? x->valuedouble : g_w / 2.0,
                        cJSON_IsNumber(y) ? y->valuedouble : g_h / 2.0);
    } else if (!strcmp(cmd, "eval")) {
        const char *js = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "js"));
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
        if (js)
            webkit_web_view_evaluate_javascript(g_view, js, -1, NULL, NULL, NULL, on_eval_done,
                                                GINT_TO_POINTER(cJSON_IsNumber(id) ? (int) id->valuedouble : 0));
    } else if (!strcmp(cmd, "resize")) {
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(j, "w"), *h = cJSON_GetObjectItemCaseSensitive(j, "h");
        if (cJSON_IsNumber(w) && cJSON_IsNumber(h) && w->valuedouble >= 16 && h->valuedouble >= 16) {
            g_w = (int) w->valuedouble;
            g_h = (int) h->valuedouble;
            if (alloc_buffers() == 0) send_buffers("buffers");
            WPEToplevel *tl = wpe_view_get_toplevel(g_wview);
            if (tl && !wpe_toplevel_resize(tl, g_w, g_h)) wpe_toplevel_resized(tl, g_w, g_h);
        }
    } else if (!strcmp(cmd, "userScript")) {
        const char *js = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "js"));
        if (js) add_user_script(js);
    } else if (!strcmp(cmd, "clearUserScripts")) {
        webkit_user_content_manager_remove_all_scripts(webkit_web_view_get_user_content_manager(g_view));
        add_user_script(CONSOLE_HOOK);
    } else if (!strcmp(cmd, "visible")) {
        wpe_view_set_visible(g_wview, !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "on")));
    } else if (!strcmp(cmd, "close")) {
        g_main_loop_quit(g_loop);
    }
}

static gboolean on_ctl(gint fd, GIOCondition cond, gpointer user) {
    (void) user;
    char buf[8192];
    ssize_t n = (cond & G_IO_IN) ? read(fd, buf, sizeof(buf)) : 0;
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0) {
        g_main_loop_quit(g_loop); /* TVF is gone */
        return G_SOURCE_REMOVE;
    }
    g_string_append_len(g_inbuf, buf, n);
    char *nl;
    while ((nl = memchr(g_inbuf->str, '\n', g_inbuf->len)) != NULL) {
        *nl = 0;
        cJSON *j = cJSON_Parse(g_inbuf->str);
        if (j) {
            handle_cmd(j);
            cJSON_Delete(j);
        } else if (g_inbuf->str[0])
            fprintf(stderr, "[tvf-web] kein JSON: %.80s\n", g_inbuf->str);
        g_string_erase(g_inbuf, 0, nl - g_inbuf->str + 1);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr, "tvf-web --fd N [--width W] [--height H] [--data-dir DIR] [--user-agent UA] [--media]\n"
                    "tvf-web --url URL --png FILE [--proxy URI] [--settle MS] [--timeout MS] [--js EXPR]\n"
                    "        [--keys CODES|mX:Y|cX:Y|wD:X:Y] [--native-keys] [--user-script FILE] [--bg RRGGBB] [--user-agent UA]\n"
                    "        [--width W] [--height H] [--data-dir DIR]\n");
}

static gboolean test_keys(gpointer user) {
    /* "37,403,13": je Taste down/up; Zeiger: "m640:360" bewegen, "c640:360"
     * klicken, "w1:640:360" Radstufe auf (w-1 ab) */
    t_keys_at = now_ms() - g_t0;
    char *list = g_strdup(user), *save = NULL;
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        double x, y;
        int d;
        if (sscanf(t, "m%lf:%lf", &x, &y) == 2) {
            send_pointer("move", x, y);
        } else if (sscanf(t, "c%lf:%lf", &x, &y) == 2) {
            send_pointer("move", x, y);
            send_pointer("down", x, y);
            send_pointer("up", x, y);
        } else if (sscanf(t, "w%d:%lf:%lf", &d, &x, &y) == 3) {
            send_scroll(d, x, y);
        } else {
            send_key(atoi(t), 1);
            send_key(atoi(t), 0);
        }
    }
    g_free(list);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    const char *data_dir = NULL, *ua = NULL, *proxy = NULL;
    int media = 0;
    g_debug = getenv("TVF_WEB_DEBUG") ? atoi(getenv("TVF_WEB_DEBUG")) : 0;
    if (getenv("TVF_WEB_DEBUG") && !g_debug) g_debug = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--fd") && v) g_ctl = atoi(argv[++i]);
        else if (!strcmp(a, "--width") && v) g_w = atoi(argv[++i]);
        else if (!strcmp(a, "--height") && v) g_h = atoi(argv[++i]);
        else if (!strcmp(a, "--data-dir") && v) data_dir = argv[++i];
        else if (!strcmp(a, "--user-agent") && v) ua = argv[++i];
        else if (!strcmp(a, "--proxy") && v) proxy = argv[++i];
        else if (!strcmp(a, "--url") && v) t_url = argv[++i];
        else if (!strcmp(a, "--png") && v) t_png = argv[++i];
        else if (!strcmp(a, "--js") && v) t_js = argv[++i];
        else if (!strcmp(a, "--keys") && v) t_keys = argv[++i];
        else if (!strcmp(a, "--user-script") && v) t_script = argv[++i];
        else if (!strcmp(a, "--settle") && v) t_settle = atoi(argv[++i]);
        else if (!strcmp(a, "--timeout") && v) t_timeout = atoi(argv[++i]);
        else if (!strcmp(a, "--bg") && v) t_bg = (uint32_t) strtoul(argv[++i], NULL, 16);
        else if (!strcmp(a, "--media")) media = 1;
        else if (!strcmp(a, "--native-keys")) g_native_all = 1;
        else {
            usage();
            return 2;
        }
    }
    if (g_ctl < 0 && !t_url) {
        usage();
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    init_bundle(data_dir);
    /* Debian bundle: Mesa llvmpipe, Skia paints on the CPU. webOS-SDK bundle:
     * GL on the TV's Mali (vendor EGL default display), read back into SHM */
    if (g_loader[0] || getenv("TVF_WEB_CPU")) setenv_default("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1");
    setenv_default("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1");
    setenv_default("GSETTINGS_BACKEND", "memory");
    setenv_default("GIO_USE_VFS", "local");
    setenv_default("GST_REGISTRY_FORK", "no");
    if (data_dir) {
        char *c = g_build_filename(data_dir, "xdg-cache", NULL), *d = g_build_filename(data_dir, "xdg-data", NULL);
        g_mkdir_with_parents(c, 0700);
        g_mkdir_with_parents(d, 0700);
        setenv("XDG_CACHE_HOME", c, 1);
        setenv("XDG_DATA_HOME", d, 1);
        g_free(c);
        g_free(d);
    }
    if (g_debug)
        fprintf(stderr, "[tvf-web] Bundle '%s', Loader '%s', Preload '%s'\n", g_bundle, g_loader, g_preload);

    g_t0 = now_ms();
    g_loop = g_main_loop_new(NULL, FALSE);
    g_inbuf = g_string_new(NULL);
    if (t_png) {
        t_frame = calloc((size_t) g_w * g_h, 4);
        t_frame_max = calloc((size_t) g_w * g_h, 4);
    }
    if (create_view(data_dir, ua, media) != 0) return 1;
    set_proxy(proxy);
    if (t_script) {
        gchar *js = NULL;
        if (g_file_get_contents(t_script, &js, NULL, NULL)) add_user_script(js);
        else fprintf(stderr, "[tvf-web] Skript %s nicht lesbar\n", t_script);
        g_free(js);
    }
    if (g_ctl >= 0) {
        fcntl(g_ctl, F_SETFD, FD_CLOEXEC);
        if (alloc_buffers() != 0) return 1;
        send_buffers("ready");
        g_unix_fd_add(g_ctl, G_IO_IN | G_IO_HUP | G_IO_ERR, on_ctl, NULL);
    } else {
        webkit_web_view_load_uri(g_view, t_url);
        t_done_src = g_timeout_add(t_timeout, test_finish, NULL);
    }
    g_main_loop_run(g_loop);
    if (g_ctl >= 0) send_json(ev("exit"), -1);
    return 0;
}
