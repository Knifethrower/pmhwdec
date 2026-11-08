/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* bench_copyout: CPU cost per frame of two ways to get a decoded NV12 picture out of the VE.
 *   tiled  = 32x32-tiled output untiled 32 bytes at a time (the Allwinner media-codec route, as GreenOvercast does
 *            it), then one more full copy into the frame handed to the renderer
 *   linear = linear NV12 from the engine copied row by row (pmhwdec's copy-out)
 * Ordinary cached memory stands in for cached ION buffers; the cache flush before the read is not measured.
 * Sources rotate over 4 frames so they do not stay in the cache.
 * usage: bench_copyout [iterations]          (prints median and best ms for 1080p and 720p) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NSRC 4
/* keeps the compiler from dropping copies whose result nothing reads */
#define USED(p) __asm__ volatile("" : : "r"(p) : "memory")

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void untile32(const uint8_t *src, uint8_t *dst, int stride, int w, int h)
{
    int tiles_per_row = (stride + 31) / 32;
    for (int y = 0; y < h; y++) {
        const uint8_t *tile_row = src + (size_t)(y / 32) * tiles_per_row * 1024 + (size_t)(y % 32) * 32;
        uint8_t *out = dst + (size_t)y * w;
        for (int x = 0; x < w; x += 32)
            memcpy(out + x, tile_row + (size_t)(x / 32) * 1024, w - x < 32 ? w - x : 32);
    }
}

static void copy_rows(const uint8_t *src, int sstride, uint8_t *dst, int dstride, int row, int rows)
{
    for (int y = 0; y < rows; y++) memcpy(dst + (size_t)y * dstride, src + (size_t)y * sstride, row);
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void report(const char *what, double *t, int n)
{
    qsort(t, n, sizeof *t, cmp);
    printf("  %-34s median %6.2f ms   best %6.2f ms\n", what, t[n / 2], t[0]);
}

static void run(int w, int h, int iters)
{
    int stride = (w + 31) & ~31, bh = (h + 63) & ~63;
    size_t luma = (size_t)stride * bh, frame = luma + luma / 2, out = (size_t)w * h * 3 / 2;
    uint8_t *src[NSRC], *lin = malloc(out), *dst = malloc(out);
    double *t = malloc(sizeof(double) * iters);
    for (int i = 0; i < NSRC; i++) {
        src[i] = aligned_alloc(4096, (frame + 4095) & ~(size_t)4095);
        for (size_t o = 0; o < frame; o++) src[i][o] = (uint8_t)(o * 7 + i);
    }
    memset(lin, 0, out);
    memset(dst, 0, out);
    printf("%dx%d (source stride %d, %zu KB a frame)\n", w, h, stride, frame / 1024);

    for (int i = 0; i < iters; i++) {
        const uint8_t *s = src[i % NSRC];
        double a = now_ms();
        untile32(s, lin, stride, w, h);
        untile32(s + luma, lin + (size_t)w * h, stride, w, h / 2);
        USED(dst); USED(lin);
        t[i] = now_ms() - a;
    }
    report("untile (Y + UV)", t, iters);

    for (int i = 0; i < iters; i++) {
        double a = now_ms();
        memcpy(dst, lin, out);
        USED(dst); USED(lin);
        t[i] = now_ms() - a;
        lin[i % out]++;
    }
    report("second copy of the untiled frame", t, iters);

    for (int i = 0; i < iters; i++) {
        const uint8_t *s = src[i % NSRC];
        double a = now_ms();
        untile32(s, lin, stride, w, h);
        untile32(s + luma, lin + (size_t)w * h, stride, w, h / 2);
        memcpy(dst, lin, out);
        USED(dst); USED(lin);
        t[i] = now_ms() - a;
    }
    report("tiled route total", t, iters);

    for (int i = 0; i < iters; i++) {
        const uint8_t *s = src[i % NSRC];
        double a = now_ms();
        copy_rows(s, stride, dst, w, w, h);
        copy_rows(s + luma, stride, dst + (size_t)w * h, w, w, h / 2);
        USED(dst); USED(lin);
        t[i] = now_ms() - a;
    }
    report("linear copy-out (pmhwdec)", t, iters);

    for (int i = 0; i < NSRC; i++) free(src[i]);
    free(lin);
    free(dst);
    free(t);
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 200;
    if (iters < 1) iters = 200;
    run(1920, 1080, iters);
    run(1280, 720, iters);
    return 0;
}
