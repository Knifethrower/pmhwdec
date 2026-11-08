// SPDX-License-Identifier: LGPL-2.1-or-later
/* Public API: picks the first backend that can decode the codec on this device. */
#include <stdlib.h>
#include <string.h>
#include "internal.h"

int pmhw_log_on;

struct PmhwDecoder {
    const Backend *be;
    void *ctx;
};

static const Backend *backends(int i)
{
    switch (i) {
    case 0: return &pmhw_cedar_client_backend;                 /* Allwinner, vendor kernels (pmhw-cedard helper) */
    case 1: return &pmhw_rkmpp_backend;                        /* Rockchip, vendor kernels (MPP) */
    case 2: return pmhw_stateless_backend(&pmhw_v4l2req_ops);  /* mainline V4L2 stateless (ROCKNIX) */
    case 3: return &pmhw_v4l2m2m_backend;                      /* mainline V4L2 stateful: Qualcomm, Amlogic */
    default: return NULL;
    }
}

static const char *forced(void)
{
    static int init;
    if (!init) { init = 1; pmhw_log_on = getenv("PMHWDEC_LOG") && *getenv("PMHWDEC_LOG") && strcmp(getenv("PMHWDEC_LOG"), "0"); }
    return getenv("PMHWDEC_BACKEND");
}

/* the backend says it can, and it decoded the self-test clips right (a failed 10-bit test caps it at 8 bit) */
static int usable(const Backend *b, PmhwCodec codec, PmhwCaps *caps)
{
    if (b->query(codec, caps)) return 0;
    int depth = pmhw_selftest(b, codec, caps->max_bit_depth);
    if (!depth) return 0;
    caps->max_bit_depth = depth;
    return 1;
}

int pmhw_query(PmhwCodec codec, PmhwCaps *caps)
{
    const char *f = forced();
    if (f && !strcmp(f, "off")) return -1;
    for (int i = 0; backends(i); i++) {
        const Backend *b = backends(i);
        if (f && *f && strcmp(f, b->name)) continue;
        if (usable(b, codec, caps)) return 0;
    }
    return -1;
}

PmhwDecoder *pmhw_open(PmhwCodec codec, int width, int height)
{
    const char *f = forced();
    if (f && !strcmp(f, "off")) return NULL;
    for (int i = 0; backends(i); i++) {
        const Backend *b = backends(i);
        PmhwCaps caps;
        if (f && *f && strcmp(f, b->name)) continue;
        if (!usable(b, codec, &caps)) continue;
        void *ctx = b->open(codec, width, height);
        if (!ctx) continue;
        PmhwDecoder *d = malloc(sizeof *d);
        if (!d) { b->close(ctx); return NULL; }
        d->be = b;
        d->ctx = ctx;
        LOG("using %s", b->name);
        return d;
    }
    LOG("no hardware decoder for codec %d", codec);
    return NULL;
}

int pmhw_send(PmhwDecoder *d, const uint8_t *data, int size, int64_t pts) { return d->be->send(d->ctx, data, size, pts); }
int pmhw_get(PmhwDecoder *d, PmhwPicture *pic) { return d->be->get(d->ctx, pic); }
void pmhw_put(PmhwDecoder *d, PmhwPicture *pic) { d->be->put(d->ctx, pic); }
void pmhw_flush(PmhwDecoder *d) { d->be->flush(d->ctx); }
const char *pmhw_backend_name(const PmhwDecoder *d) { return d->be->name; }

void pmhw_close(PmhwDecoder *d)
{
    if (!d) return;
    d->be->close(d->ctx);
    free(d);
}
