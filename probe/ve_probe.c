/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ve_probe: reads the Allwinner video engine's version registers through the vendor kernel's /dev/cedar_dev,
 * with no Allwinner userspace code (no libcedarc, no blobs). Tells whether a chip's engine is the generation
 * mainline Cedrus drives (H6/H616 family), which decides if the blob-free decoder can work on it.
 *
 * Interface taken from the GPL-2.0 vendor 4.9 driver (drivers/media/cedar-ve/cedar_ve.{c,h}: A133 copy in
 * CrealityTech/sonic_pad_os, H616 copy in orangepi-xunlong/linux-orangepi orange-pi-4.9-sun50iw9): ioctl
 * numbers are the raw enum values, mmap maps the MACC register block (0x01C0E000) whatever the offset.
 * Register offsets from mainline cedrus_regs.h (VE_VERSION 0xf0, >> 16), plus 0xe0/0xe4, which newer engines
 * use for the version according to the blobs' own error messages.
 *
 * Only reads ID/mode registers, with the engine clock on; releases everything it took. Do not run it while a
 * video is playing (it shares the engine's clock reference count with the decoder).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* cedar_ve.h, enum IOCTL_CMD */
#define IOCTL_GET_ENV_INFO 0x101
#define IOCTL_ENABLE_VE    0x105
#define IOCTL_DISABLE_VE   0x106
#define IOCTL_ENGINE_REQ   0x206
#define IOCTL_ENGINE_REL   0x207
#define IOCTL_GET_IC_VER   0x209

#define VE_MODE    0x00
#define VE_VERSION 0xf0
#define MAP_SIZE   0x1000

static uint32_t rd(volatile uint8_t *regs, unsigned off) { return *(volatile uint32_t *)(regs + off); }

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/cedar_dev";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("open %s: %s\n", dev, strerror(errno)); return 1; }

    struct { unsigned int phymem_start; int phymem_total_size; unsigned long address_macc; } env;
    memset(&env, 0, sizeof env);
    int r = ioctl(fd, IOCTL_GET_ENV_INFO, &env);
    printf("GET_ENV_INFO: ret %d, phymem 0x%x+%d, macc 0x%lx\n", r, env.phymem_start, env.phymem_total_size,
           env.address_macc);
    printf("GET_IC_VER: ret %d\n", ioctl(fd, IOCTL_GET_IC_VER, 0));

    /* 4.9 driver without USE_CEDAR_ENGINE: ref_count++ and the hardware clocks on (first user) */
    r = ioctl(fd, IOCTL_ENGINE_REQ, 0);
    if (r < 0) { printf("ENGINE_REQ: %s (driver built with USE_CEDAR_ENGINE?)\n", strerror(errno)); close(fd); return 1; }
    int enabled = ioctl(fd, IOCTL_ENABLE_VE, 0) == 0;
    printf("ENGINE_REQ ok, ENABLE_VE %s\n", enabled ? "ok" : strerror(errno));

    int rc = 0;
    volatile uint8_t *regs = mmap(NULL, MAP_SIZE, PROT_READ, MAP_SHARED, fd,
                                  env.address_macc && env.address_macc % 4096 == 0 ? (off_t)env.address_macc : 0);
    if (regs == MAP_FAILED) {
        printf("mmap: %s\n", strerror(errno));
        rc = 1;
    } else {
        uint32_t v = rd(regs, VE_VERSION), e0 = rd(regs, 0xe0), e4 = rd(regs, 0xe4);
        printf("VE_MODE   (0x00) = 0x%08x\n", rd(regs, VE_MODE));
        printf("reg 0xe0         = 0x%08x\n", e0);
        printf("reg 0xe4         = 0x%08x\n", e4);
        printf("VE_VERSION(0xf0) = 0x%08x -> version 0x%04x\n", v, v >> 16);
        /* one line for scripts / DESIGN.md */
        printf("RESULT version=0x%04x f0=0x%08x e0=0x%08x e4=0x%08x\n", v >> 16, v, e0, e4);
        munmap((void *)regs, MAP_SIZE);
    }

    if (enabled) ioctl(fd, IOCTL_DISABLE_VE, 0);
    ioctl(fd, IOCTL_ENGINE_REL, 0);
    close(fd);
    return rc;
}
