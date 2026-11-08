/* SPDX-License-Identifier: GPL-3.0-or-later */
/* EGL/GLES capabilities of the webOS TV (for a later GPU path of tvf-web).
 * Build with the webOS SDK (softfp, TV libraries), run in the dev shell. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

static const char *want_egl[] = {"EGL_WL_bind_wayland_display", "EGL_KHR_image_base", "EGL_KHR_image",
                                 "EGL_KHR_surfaceless_context", "EGL_EXT_image_dma_buf_import",
                                 "EGL_KHR_gl_texture_2D_image", "EGL_KHR_fence_sync", "EGL_ANDROID_native_fence_sync",
                                 "EGL_KHR_platform_wayland", "EGL_EXT_platform_wayland", "EGL_MESA_platform_surfaceless",
                                 NULL};
static const char *want_gl[] = {"GL_OES_EGL_image", "GL_OES_EGL_image_external", "GL_EXT_texture_format_BGRA8888",
                                "GL_EXT_unpack_subimage", "GL_OES_packed_depth_stencil", NULL};

static void check(const char *what, const char *ext, const char **want) {
    printf("%s:\n", what);
    for (int i = 0; want[i]; i++) {
        const char *p = ext ? strstr(ext, want[i]) : NULL;
        size_t n = strlen(want[i]);
        int ok = p && (p[n] == ' ' || p[n] == 0);
        printf("  %-34s %s\n", want[i], ok ? "yes" : "no");
    }
}

int main(void) {
    const char *cext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("EGL client extensions: %s\n\n", cext ? cext : "(none)");
    struct wl_display *wl = wl_display_connect(NULL);
    printf("wayland connect: %s\n", wl ? "ok" : "failed");
    EGLDisplay d = eglGetDisplay(wl ? (EGLNativeDisplayType) wl : EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) {
        printf("eglInitialize failed (0x%x)\n", eglGetError());
        if (wl) {
            d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) return 1;
            printf("EGL_DEFAULT_DISPLAY works\n");
        } else
            return 1;
    }
    const char *ext = eglQueryString(d, EGL_EXTENSIONS);
    printf("EGL %d.%d vendor=%s version=%s apis=%s\n", maj, min, eglQueryString(d, EGL_VENDOR),
           eglQueryString(d, EGL_VERSION), eglQueryString(d, EGL_CLIENT_APIS));
    printf("EGL extensions: %s\n\n", ext);
    check("EGL", ext, want_egl);
    printf("eglBindWaylandDisplayWL proc: %p\n", (void *) eglGetProcAddress("eglBindWaylandDisplayWL"));
    printf("eglCreateImageKHR proc: %p\n", (void *) eglGetProcAddress("eglCreateImageKHR"));
    printf("glEGLImageTargetTexture2DOES proc: %p\n", (void *) eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    int pbuf = 1;
    if (!eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || n < 1) {
        cfg_attr[3] = EGL_WINDOW_BIT;
        pbuf = 0;
        if (!eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || n < 1) {
            printf("no ES2 config\n");
            return 1;
        }
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (c == EGL_NO_CONTEXT) {
        printf("eglCreateContext failed 0x%x\n", eglGetError());
        return 1;
    }
    EGLSurface s = EGL_NO_SURFACE;
    int cur = eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
    printf("surfaceless make current: %s\n", cur ? "ok" : "failed");
    if (!cur && pbuf) {
        EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        s = eglCreatePbufferSurface(d, cfg, pa);
        cur = s != EGL_NO_SURFACE && eglMakeCurrent(d, s, s, c);
        printf("pbuffer make current: %s\n", cur ? "ok" : "failed");
    }
    if (cur) {
        const char *gext = (const char *) glGetString(GL_EXTENSIONS);
        printf("GL vendor=%s renderer=%s version=%s glsl=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
               glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
        GLint mx = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mx);
        printf("GL_MAX_TEXTURE_SIZE %d\nGL extensions: %s\n\n", mx, gext);
        check("GL", gext, want_gl);
    }
    eglTerminate(d);
    if (wl) wl_display_disconnect(wl);
    return 0;
}
