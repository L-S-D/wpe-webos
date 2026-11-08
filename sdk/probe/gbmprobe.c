/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Zero-copy groundwork: can a jailed process allocate dma-buf backed buffers
 * with the TV's Mali GBM and import them into EGL? Tries /dev/mali0 and
 * /dev/dma_buf_unified as GBM device fds (there is no /dev/dri in the jail). */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct gbm_device;
struct gbm_bo;
#define GBM_FORMAT_ARGB8888 0x34325241
#define GBM_BO_USE_RENDERING (1 << 2)
#define GBM_BO_USE_LINEAR (1 << 4)

int main(void) {
    void *g = dlopen("libgbm.so.1", RTLD_NOW);
    if (!g) {
        printf("libgbm: %s\n", dlerror());
        return 1;
    }
    struct gbm_device *(*create)(int) = dlsym(g, "gbm_create_device");
    struct gbm_bo *(*bo_create)(struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t) = dlsym(g, "gbm_bo_create");
    int (*bo_fd)(struct gbm_bo *) = dlsym(g, "gbm_bo_get_fd");
    uint32_t (*bo_stride)(struct gbm_bo *) = dlsym(g, "gbm_bo_get_stride");
    const char *(*backend)(struct gbm_device *) = dlsym(g, "gbm_device_get_backend_name");
    printf("gbm_create_device %p, gbm_bo_create %p, gbm_bo_get_fd %p\n", (void *) create, (void *) bo_create,
           (void *) bo_fd);
    const char *devs[] = {"/dev/mali0", "/dev/dma_buf_unified", NULL};
    for (int i = 0; devs[i]; i++) {
        int fd = open(devs[i], O_RDWR | O_CLOEXEC);
        printf("%s: open %s\n", devs[i], fd >= 0 ? "ok" : "fehlgeschlagen");
        if (fd < 0 || !create) continue;
        struct gbm_device *d = create(fd);
        printf("  gbm_create_device -> %p%s%s\n", (void *) d, d && backend ? " backend " : "",
               d && backend ? backend(d) : "");
        if (!d || !bo_create) continue;
        struct gbm_bo *bo = bo_create(d, 1280, 720, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!bo) bo = bo_create(d, 1280, 720, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING);
        printf("  gbm_bo_create 1280x720 ARGB8888 -> %p", (void *) bo);
        if (bo && bo_fd) printf(", dma-buf fd %d, stride %u", bo_fd(bo), bo_stride ? bo_stride(bo) : 0);
        printf("\n");
        /* EGL on this GBM device */
        PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (gpd) {
            EGLDisplay ed = gpd(EGL_PLATFORM_GBM_KHR, d, NULL);
            EGLint a, b;
            printf("  EGL auf GBM: display %p, initialize %s\n", ed,
                   ed != EGL_NO_DISPLAY && eglInitialize(ed, &a, &b) ? "ok" : "fehlgeschlagen");
        }
    }
    return 0;
}
