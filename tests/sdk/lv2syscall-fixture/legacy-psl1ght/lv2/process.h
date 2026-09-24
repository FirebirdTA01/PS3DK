#ifndef __LV2_PROCESS_H__
#define __LV2_PROCESS_H__
#include <ppu-types.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void sysProcessExit(int status) __attribute__((noreturn));
void sysProcessExitSpawn2(const char *path, const char *argv[], const char *envp[], void *data, size_t size, int priority, u64 flags);
#ifdef __cplusplus
}
#endif
#endif
