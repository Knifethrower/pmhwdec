/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exp-Golomb bit reader over RBSP (emulation prevention bytes already removed). */
#ifndef PMHW_BITS_H
#define PMHW_BITS_H
#include <stdint.h>

typedef struct { const uint8_t *d; int n, pos; } Bits;

static inline unsigned u(Bits *b, int k)
{
    unsigned v = 0;
    while (k--) { v = v << 1 | (b->pos < b->n * 8 ? b->d[b->pos >> 3] >> (7 - (b->pos & 7)) & 1 : 0); b->pos++; }
    return v;
}
static inline unsigned ue(Bits *b) { int z = 0; while (!u(b, 1) && z < 32) z++; return (1u << z) - 1 + u(b, z); }
static inline int se(Bits *b) { unsigned k = ue(b); return k & 1 ? (int)(k + 1) / 2 : -(int)(k / 2); }
static inline int bits_overrun(const Bits *b) { return b->pos > b->n * 8; }
static inline int more_rbsp(const Bits *b)
{
    int last = b->n * 8 - 1;
    while (last >= 0 && !(b->d[last >> 3] >> (7 - (last & 7)) & 1)) last--;   /* rbsp stop bit */
    return b->pos < last;
}
static inline int ceil_log2(int v) { int r = 0; while ((1 << r) < v) r++; return r; }

#endif
