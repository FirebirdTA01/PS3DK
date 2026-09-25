#ifndef PS3TC_HEAP_HOST_H
#define PS3TC_HEAP_HOST_H
#include <stddef.h>
#include <stdint.h>
typedef int32_t s32;
typedef uint32_t u32;
typedef uintptr_t sys_mem_addr_t;
typedef uintptr_t sys_addr_t;
typedef char *caddr_t;
struct _reent { int _errno; };
typedef struct { uint32_t total_user_memory, available_user_memory; } sys_memory_info_t;
#define SYS_MEMORY_PAGE_SIZE_1M UINT64_C(0x400)
int sys_memory_allocate(uint32_t, uint64_t, sys_addr_t *);
int sys_memory_free(sys_addr_t);
int sys_memory_get_user_memory_size(sys_memory_info_t *);
int sysTtyWrite(int, const void *, uint32_t, uint32_t *);
void sysProcessExit(int) __attribute__((noreturn));
#endif
