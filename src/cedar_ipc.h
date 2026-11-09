/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The protocol between libpmhwdec's "cedar" backend (cedar_client.c, in the caller's process) and pmhwdec-cedard
 * (cedard.c + cedar.c, a separate GPL program that owns /dev/cedar_dev). One helper per decoder, over a
 * socketpair: every request gets one reply, except PUT. Pictures cross as dma-buf fds (SCM_RIGHTS on GET replies);
 * the client maps each buffer once and keeps the mapping. Keeping the Cedrus-derived code in its own process lets
 * closed programs (Unity players) use the library: they talk to a separate program, they don't link it. */
#ifndef PMHW_CEDAR_IPC_H
#define PMHW_CEDAR_IPC_H
#include <stdint.h>

#define CD_VERSION 1
#define CD_FD 3                  /* the helper's end of the socketpair */

enum { CD_QUERY = 1, CD_OPEN, CD_SEND, CD_GET, CD_PUT, CD_FLUSH };

typedef struct CdReq {
    uint32_t version, op;
    int32_t codec, width, height;
    int32_t max_depth;           /* OPEN: the deepest bit depth the client's self-test allows (8 or 10) */
    int64_t pts;
    uint32_t len;                /* SEND: access unit bytes following this header (0 = end of stream) */
    uint32_t token;              /* PUT: the picture's token from its GET reply */
} CdReq;

typedef struct CdRep {
    int32_t status;              /* QUERY/OPEN: 0 or -1; SEND: PMHW_*; GET: 1 picture, 0 none */
    int32_t max_width, max_height, max_bit_depth;      /* QUERY */
    /* GET: the picture; its dma-buf comes as the message's SCM_RIGHTS fd */
    int32_t width, height, left, top, stride0, stride1, corrupt;
    uint32_t offset0, offset1, fourcc, token, gen;     /* gen: bumps when the helper reallocated its buffers */
    uint64_t modifier;
    int64_t pts;
} CdRep;

/* the same layout on armhf and aarch64: a 32-bit libpmhwdec starts the 64-bit pmhwdec-cedard on a 64-bit kernel (the
 * vendor video driver and ION then see a native caller, no 32-bit ioctl translation) */
#include <stddef.h>
_Static_assert(sizeof(CdReq) == 40 && offsetof(CdReq, pts) == 24 && offsetof(CdReq, len) == 32, "CdReq layout");
_Static_assert(sizeof(CdRep) == 80 && offsetof(CdRep, modifier) == 64 && offsetof(CdRep, pts) == 72, "CdRep layout");

#endif
