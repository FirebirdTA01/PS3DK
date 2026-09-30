/* Host stand-in for <sys/ppu_thread.h>: what the PPU libdaisy headers use. */
#ifndef DAISY_HOST_SYS_PPU_THREAD_H
#define DAISY_HOST_SYS_PPU_THREAD_H
#include <sched.h>
static inline int sys_ppu_thread_yield(void) { return sched_yield(); }
#endif
