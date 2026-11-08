/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Can a child process of a dev-mode/app process use the TV's Mali EGL
 * offscreen? Tries EGL_DEFAULT_DISPLAY and the Wayland display, each in a
 * forked child: surfaceless GLES context, FBO, clear, glReadPixels (BGRA as
 * WebKit's SHM render target does), readback speed at 1280x720. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void libs(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512], last[256] = "";
    printf("  geladene Bibliotheken:");
    while (f && fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '/');
        if (!p || !strstr(p, ".so")) continue;
        p[strcspn(p, "\n")] = 0;
        char *b = strrchr(p, '/') + 1;
        if (!strcmp(b, last)) continue;
        snprintf(last, sizeof(last), "%s", b);
        printf(" %s", p);
    }
    printf("\n");
    if (f) fclose(f);
}

static int run(int mode) {
    struct wl_display *wl = NULL;
    EGLDisplay d;
    if (mode == 1) {
        wl = wl_display_connect(NULL);
        if (!wl) {
            printf("  wl_display_connect fehlgeschlagen\n");
            return 2;
        }
        d = eglGetDisplay((EGLNativeDisplayType) wl);
    } else
        d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) {
        printf("  eglInitialize fehlgeschlagen 0x%x\n", eglGetError());
        return 3;
    }
    printf("  EGL %d.%d %s\n", maj, min, eglQueryString(d, EGL_VERSION));
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                   EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n < 1) {
        printf("  keine Konfiguration\n");
        return 4;
    }
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
        printf("  Kontext/surfaceless fehlgeschlagen 0x%x\n", eglGetError());
        return 5;
    }
    printf("  GL_RENDERER %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    int W = 1280, H = 720;
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("  FBO unvollstaendig\n");
        return 6;
    }
    unsigned char *px = malloc((size_t) W * H * 4);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(10, 10, 20, 20);
    glClearColor(1, 0, 0, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    GLenum e = glGetError();
    printf("  readPixels BGRA: err 0x%x, Pixel(0,0) B%d G%d R%d A%d, Pixel(15,15) B%d G%d R%d A%d\n", e, px[0], px[1],
           px[2], px[3], px[(15 * W + 15) * 4], px[(15 * W + 15) * 4 + 1], px[(15 * W + 15) * 4 + 2],
           px[(15 * W + 15) * 4 + 3]);
    double t = now_ms();
    for (int i = 0; i < 20; i++) {
        glClearColor(i / 20.0f, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    }
    printf("  clear+readPixels 1280x720: %.1f ms je Bild\n", (now_ms() - t) / 20);
    libs();
    return 0;
}

int main(int argc, char **argv) {
    for (int mode = 0; mode < 2; mode++) {
        printf("%s (XDG_RUNTIME_DIR=%s, WAYLAND_DISPLAY=%s):\n", mode ? "Wayland-Display" : "EGL_DEFAULT_DISPLAY",
               getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "-",
               getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "-");
        fflush(stdout);
        pid_t p = fork();
        if (!p) {
            int r = run(mode);
            fflush(stdout);
            _exit(r);
        }
        int st = 0;
        waitpid(p, &st, 0);
        printf("  -> %s %d\n", WIFEXITED(st) ? "exit" : "Signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
    }
    return 0;
}
