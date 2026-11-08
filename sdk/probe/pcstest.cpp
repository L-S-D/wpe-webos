/* SPDX-License-Identifier: GPL-3.0-or-later */
// Does the Skia raster pipeline stage ABI (pcs("aapcs-vfp") + GCC vector types, called through a
// function pointer) survive a softfp build? Mimics SkRasterPipeline_opts.h (narrow stages, ARM32 NEON).
#include <cstdio>
#include <cstring>
template <int N, typename T> struct VecHelper { typedef T __attribute__((vector_size(N * sizeof(T)))) V; };
template <int N, typename T> using Vec = typename VecHelper<N, T>::V;
using F = Vec<4, float>;
#ifdef NO_PCS
#define ABI
#else
#define ABI __attribute__((pcs("aapcs-vfp")))
#endif
struct Stage { void* fn; void* ctx; };
struct Params { size_t dx, dy; void* base; F dr, dg, db, da; };
using Fn = void(ABI*)(Params*, Stage* program, F r, F g, F b, F a);
__attribute__((noinline)) static void ABI last(Params* p, Stage* program, F r, F g, F b, F a) {
    float* out = (float*) program->ctx;
    memcpy(out, &r, 16); memcpy(out + 4, &g, 16); memcpy(out + 8, &b, 16); memcpy(out + 12, &a, 16);
}
__attribute__((noinline)) static void ABI first(Params* p, Stage* program, F r, F g, F b, F a) {
    r = r + 0.5f;
    auto next = (Fn) (++program)->fn;
    next(p, program, r, g, b, a);
}
int main() {
    float out[16] = {0};
    Stage prog[2] = { { (void*) first, nullptr }, { (void*) last, out } };
    Params p {};
    F r = {1, 2, 3, 4}, g = {5, 6, 7, 8}, b = {9, 10, 11, 12}, a = {13, 14, 15, 16};
    auto start = (Fn) prog[0].fn;
    start(&p, prog, r, g, b, a);
    int ok = 1;
    for (int i = 0; i < 16; i++) { float want = i + 1 + (i < 4 ? 0.5f : 0); if (out[i] != want) ok = 0; printf("%g ", out[i]); }
    printf("\n%s\n", ok ? "OK" : "FALSCH");
    return !ok;
}
