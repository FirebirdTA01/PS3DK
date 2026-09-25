/*
 * PS3 Custom Toolchain — <sys/tty.h>
 *
 * TTY management Lv2 syscall interface.
 * Implements canonical reference-SDK prototypes and constants,
 * with inline syscall forwarders and PSL1GHT compatibility aliases.
 */

#ifndef __PS3DK_SYS_TTY_H__
#define __PS3DK_SYS_TTY_H__

#include <stdint.h>
#include <ppu-types.h>
#include <sys/lv2_syscall.h>
#include <sys/return_code.h>

#define SYS_TTYP_MAX  (0x10)

#define SYS_TTYP0     (0)
#define SYS_TTYP1     (1)
#define SYS_TTYP2     (2)
#define SYS_TTYP3     (3)
#define SYS_TTYP4     (4)
#define SYS_TTYP5     (5)
#define SYS_TTYP6     (6)
#define SYS_TTYP7     (7)
#define SYS_TTYP8     (8)
#define SYS_TTYP9     (9)
#define SYS_TTYP10   (10)
#define SYS_TTYP11   (11)
#define SYS_TTYP12   (12)
#define SYS_TTYP13   (13)
#define SYS_TTYP14   (14)
#define SYS_TTYP15   (15)

#define SYS_TTYP_PPU_STDIN    (SYS_TTYP0)
#define SYS_TTYP_PPU_STDOUT   (SYS_TTYP0)
#define SYS_TTYP_PPU_STDERR   (SYS_TTYP1)
#define SYS_TTYP_SPU_STDOUT   (SYS_TTYP2)
#define SYS_TTYP_USER1        (SYS_TTYP3)
#define SYS_TTYP_USER2        (SYS_TTYP4)
#define SYS_TTYP_USER3        (SYS_TTYP5)
#define SYS_TTYP_USER4        (SYS_TTYP6)
#define SYS_TTYP_USER5        (SYS_TTYP7)
#define SYS_TTYP_USER6        (SYS_TTYP8)
#define SYS_TTYP_USER7        (SYS_TTYP9)
#define SYS_TTYP_USER8        (SYS_TTYP10)
#define SYS_TTYP_USER9        (SYS_TTYP11)
#define SYS_TTYP_USER10       (SYS_TTYP12)
#define SYS_TTYP_USER11       (SYS_TTYP13)
#define SYS_TTYP_USER12       (SYS_TTYP14)
#define SYS_TTYP_USER13       (SYS_TTYP15)

#ifdef __cplusplus
extern "C" {
#endif

static inline int sys_tty_write(unsigned int ch, const void *buf,
                                unsigned int len, unsigned int *pwritelen)
{
    lv2syscall4(403, (uint64_t)ch, (uint64_t)buf, (uint64_t)len, (uint64_t)pwritelen);
    return_to_user_prog(int);
}

static inline int sys_tty_read(unsigned int ch, void *buf,
                               unsigned int len, unsigned int *preadlen)
{
    lv2syscall4(402, (uint64_t)ch, (uint64_t)buf, (uint64_t)len, (uint64_t)preadlen);
    return_to_user_prog(int);
}

/* ---- PSL1GHT compatibility aliases ------------------------------ */
static inline int sysTtyWrite(int32_t channel, const void *ptr,
                              uint32_t len, uint32_t *written)
{
    return sys_tty_write((unsigned int)channel, ptr, (unsigned int)len, (unsigned int *)written);
}

static inline int sysTtyRead(int32_t channel, void *ptr,
                             uint32_t len, uint32_t *read)
{
    return sys_tty_read((unsigned int)channel, ptr, (unsigned int)len, (unsigned int *)read);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_TTY_H__ */
