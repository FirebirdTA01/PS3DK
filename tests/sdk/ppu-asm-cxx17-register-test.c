/*
 * Expands every statement-expression macro of <sys/ppu-asm.h> (with
 * -DUSE_SYS_PPU_ASM) or of <ppu-asm.h> (without), so a 'register'
 * storage class on one of their temporaries is diagnosed in C++17.
 * The <ppu-asm.h> variant also pulls <sys/spu.h>, whose raw-SPU
 * accessors use __read32/__write32 and whose syscall wrappers keep
 * their explicit-register variables (register T x __asm__("N")),
 * which -Wregister exempts.
 */
#include <ppu-types.h>
#include <stdint.h>
#ifdef USE_SYS_PPU_ASM
#include <sys/ppu-asm.h>
#else
#include <ppu-asm.h>
#include <sys/spu.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

unsigned char t_read8(const volatile void *p) { return __read8(p); }
unsigned short t_read16(const volatile void *p) { return __read16(p); }
unsigned int t_read32(const volatile void *p) { return __read32(p); }
unsigned long long t_read64(const volatile void *p) { return __read64(p); }

void t_write(volatile void *p, unsigned long long v)
{
	__write8(p, (unsigned char)v);
	__write16(p, (unsigned short)v);
	__write32(p, (unsigned int)v);
	__write64(p, v);
}

unsigned long long t_gettime(void) { return __gettime(); }

/* The descriptor EA goes in as an integer: both variants narrow it
 * through an integer type, which a pointer would make an ILP32
 * different-size cast in C. */
unsigned int t_build_opd32(const void *opd_in, uintptr_t opd_out)
{
	return __build_opd32(opd_in, opd_out);
}

#ifndef USE_SYS_PPU_ASM
u32 t_raw_spu(sys_raw_spu_t spu, u32 reg, u32 value)
{
	sysSpuRawWriteProblemStorage(spu, reg, value);
	sysSpuRawWriteLocalStorage(spu, reg, value);
	sysSpuThreadWriteProblemStorage(spu, reg, value);
	return sysSpuRawReadProblemStorage(spu, reg)
	     + sysSpuRawReadLocalStorage(spu, reg)
	     + sysSpuThreadReadProblemStorage(spu, reg);
}

s32 t_syscall(sys_spu_group_t group, u8 spup)
{
	return sysSpuThreadGroupDisonnectEventAllThreads(group, spup);
}
#endif

#ifdef __cplusplus
}
#endif
