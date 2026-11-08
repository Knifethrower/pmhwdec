// SPDX-License-Identifier: LGPL-2.1-or-later
/* pmhw-test: decodes an Annex-B H.264 / HEVC file or an IVF VP8 file through the public pmhwdec API only.
 * Prints one line per picture in OUTPUT order: "FRAME <n> 0 <n> <pts> ... adler=<hex>" (FFmpeg framecrc's adler32 of
 * the visible NV12), so `ffmpeg -i f -pix_fmt nv12 -f framecrc` compares line by line (probe/compare_crc.py).
 * Usage: pmhw-test [-c h264|hevc] [-n max_access_units] [-o first_frames.nv12 -d count] file
 *        pmhw-test -q      (capabilities only) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pmhwdec.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static uint32_t adler(const uint8_t *p, int n, uint32_t s)
{
    uint32_t a = s & 0xffff, c = s >> 16;
    for (int i = 0; i < n; i++) { a = (a + p[i]) % 65521; c = (c + a) % 65521; }
    return c << 16 | a;
}

/* is this NAL the start of a new access unit, given the current AU already has a picture (VCL) NAL? */
static int au_start(int hevc, const uint8_t *nal, int len)
{
    if (len < 3) return 0;
    if (!hevc) {
        int t = nal[0] & 0x1f;
        if (t == 1 || t == 5) return nal[1] & 0x80;           /* first_mb_in_slice == 0 */
        return t == 6 || t == 7 || t == 8 || t == 9 || (t >= 14 && t <= 18);
    }
    int t = nal[0] >> 1 & 0x3f;
    if (t <= 31) return nal[2] & 0x80;                         /* first_slice_segment_in_pic_flag */
    return t == 32 || t == 33 || t == 34 || t == 35 || t == 39 || (t >= 41 && t <= 44) || (t >= 48 && t <= 55);
}
static int is_vcl(int hevc, const uint8_t *nal) { return hevc ? (nal[0] >> 1 & 0x3f) <= 31 : ((nal[0] & 0x1f) == 1 || (nal[0] & 0x1f) == 5); }

static int frames, speed_only;
static double get_time, copy_time;
static FILE *dump;
static int dump_left;
static uint8_t *copybuf;

static void drain(PmhwDecoder *d)
{
    PmhwPicture pic;
    for (;;) {
        double t0 = now();
        int r = pmhw_get(d, &pic);
        get_time += now() - t0;
        if (r != 1) break;
        if (speed_only) { frames++; pmhw_put(d, &pic); continue; }   /* -s: decode throughput only */
        /* copy-out, as a CPU consumer would do it (whole rows from possibly uncached memory) */
        t0 = now();
        int w = pic.width, h = pic.height;
        uint8_t *dst = copybuf;
        for (int y = 0; y < h; y++, dst += w) memcpy(dst, pic.plane[0] + (pic.top + y) * pic.stride[0] + pic.left, w);
        for (int y = 0; y < h / 2; y++, dst += w) memcpy(dst, pic.plane[1] + (pic.top / 2 + y) * pic.stride[1] + pic.left, w);
        copy_time += now() - t0;
        uint32_t ad = adler(copybuf, w * h * 3 / 2, 0);
        if (dump && dump_left > 0) { fwrite(copybuf, 1, w * h * 3 / 2, dump); dump_left--; }
        printf("FRAME %d 0 %d pts %lld %dx%d%s adler=%08x\n", frames, frames, (long long)pic.pts, w, h, pic.corrupt ? " CORRUPT" : "", ad);
        frames++;
        pmhw_put(d, &pic);
    }
}

static void print_result(int errors, int aus, int agains, double send_time, double total)
{
    printf("RESULT %s access_units=%d frames=%d send_errors=%d agains=%d per frame: send %.2f ms, get %.3f ms, copy-out %.2f ms, total %.2f ms (%.1f fps)\n",
           errors ? "ERRORS" : "OK", aus, frames, errors, agains, send_time * 1e3 / (frames ? frames : 1),
           get_time * 1e3 / (frames ? frames : 1), copy_time * 1e3 / (frames ? frames : 1), total * 1e3 / (frames ? frames : 1),
           frames / (total > 0 ? total : 1));
}

/* VP8 in IVF: a 32-byte file header (its length at offset 6), then a 12-byte header before each frame */
static int run_ivf(const uint8_t *f, long n, int max_au)
{
    PmhwDecoder *d = pmhw_open(PMHW_CODEC_VP8, 0, 0);
    if (!d) { printf("RESULT NOHW no hardware decoder\n"); return 1; }
    printf("backend %s\n", pmhw_backend_name(d));
    double send_time = 0, t_start = now();
    int aus = 0, errors = 0, agains = 0;
    for (long pos = f[6] | f[7] << 8; pos + 12 <= n && aus < max_au;) {
        long len = f[pos] | f[pos + 1] << 8 | f[pos + 2] << 16 | (long)f[pos + 3] << 24;
        if (pos + 12 + len > n) break;
        double t0 = now();
        int r, stuck = 0, before = frames;
        while ((r = pmhw_send(d, f + pos + 12, (int)len, aus)) == PMHW_AGAIN) {
            send_time += now() - t0;
            agains++;
            drain(d);
            if (frames == before && ++stuck > 20000) { printf("RESULT STUCK: the decoder never takes access unit %d\n", aus); return 4; }
            t0 = now();
        }
        send_time += now() - t0;
        if (r == PMHW_UNSUPPORTED) {
            printf("RESULT UNSUPPORTED after %d access units, %d frames (use software for this stream)\n", aus, frames);
            pmhw_close(d);
            return 3;
        }
        if (r != PMHW_OK) errors++;
        aus++;
        drain(d);
        pos += 12 + len;
    }
    pmhw_send(d, NULL, 0, 0);
    drain(d);
    double total = now() - t_start;
    pmhw_close(d);
    if (dump) fclose(dump);
    print_result(errors, aus, agains, send_time, total);
    return errors ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *codec_name = NULL, *dump_path = NULL;
    int max_au = 1 << 30, query = 0;
    dump_left = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) codec_name = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_au = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) dump_path = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dump_left = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q")) query = 1;
        else if (!strcmp(argv[i], "-s")) speed_only = 1;
        else path = argv[i];
    }
    if (query) {
        const char *names[] = { "H.264", "HEVC", "VP9", "VP8" };
        for (int c = 0; c < PMHW_CODEC_COUNT; c++) {
            PmhwCaps caps;
            if (pmhw_query(c, &caps)) printf("%s: no hardware\n", names[c]);
            else printf("%s: %s, up to %dx%d, %d-bit\n", names[c], caps.backend, caps.max_width, caps.max_height, caps.max_bit_depth);
        }
        return 0;
    }
    if (!path) { fprintf(stderr, "usage: %s [-c h264|hevc] [-n max_au] [-o dump.nv12 -d n] file | -q\n", argv[0]); return 2; }
    int hevc = codec_name ? !strcmp(codec_name, "hevc") : (strstr(path, ".hevc") || strstr(path, ".h265") || strstr(path, ".265"));
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *es = malloc(n);
    if (!es || fread(es, 1, n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    if (dump_path) dump = fopen(dump_path, "wb");
    copybuf = malloc(4096 * 2304 * 3 / 2);
    if (n > 32 && !memcmp(es, "DKIF", 4)) return run_ivf(es, n, max_au);

    PmhwDecoder *d = pmhw_open(hevc ? PMHW_CODEC_HEVC : PMHW_CODEC_H264, 0, 0);
    if (!d) { printf("RESULT NOHW no hardware decoder\n"); return 1; }
    printf("backend %s\n", pmhw_backend_name(d));

    /* NAL units: start code position (incl. a 4th zero byte) and payload position */
    long cap = 1024, cnt = 0, *st = malloc(cap * sizeof *st), *pay = malloc(cap * sizeof *pay);
    for (long i = 0; i + 3 <= n; i++) {
        if (!(es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1)) continue;
        if (cnt == cap) { cap *= 2; st = realloc(st, cap * sizeof *st); pay = realloc(pay, cap * sizeof *pay); }
        st[cnt] = i > 0 && es[i - 1] == 0 ? i - 1 : i;
        pay[cnt++] = i + 3;
        i += 2;
    }
    double send_time = 0, t_start = now();
    int aus = 0, errors = 0, agains = 0;
    long au_begin = cnt ? st[0] : 0;
    int has_vcl = 0;
    for (long k = 0; k <= cnt && aus < max_au; k++) {
        int boundary = k == cnt;
        if (!boundary) {
            long len = (k + 1 < cnt ? st[k + 1] : n) - pay[k];
            if (has_vcl && au_start(hevc, es + pay[k], (int)len)) boundary = 1;
        }
        if (boundary && has_vcl) {
            long end = k < cnt ? st[k] : n;
            double t0 = now();
            int r;
            int stuck = 0, before = frames;
            while ((r = pmhw_send(d, es + au_begin, (int)(end - au_begin), aus)) == PMHW_AGAIN) {
                send_time += now() - t0;
                agains++;
                drain(d);
                if (frames == before && ++stuck > 20000) { printf("RESULT STUCK: the decoder never takes access unit %d\n", aus); return 4; }
                t0 = now();
            }
            send_time += now() - t0;
            if (r == PMHW_UNSUPPORTED) {
                printf("RESULT UNSUPPORTED after %d access units, %d frames (use software for this stream)\n", aus, frames);
                pmhw_close(d);
                return 3;
            }
            if (r != PMHW_OK) errors++;
            aus++;
            drain(d);
            au_begin = end;
            has_vcl = 0;
        }
        if (k < cnt && is_vcl(hevc, es + pay[k])) has_vcl = 1;
    }
    pmhw_send(d, NULL, 0, 0);
    drain(d);
    double total = now() - t_start;
    pmhw_close(d);
    if (dump) fclose(dump);
    print_result(errors, aus, agains, send_time, total);
    return errors ? 1 : 0;
}
