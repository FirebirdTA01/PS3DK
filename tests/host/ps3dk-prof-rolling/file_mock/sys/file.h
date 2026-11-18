/* Test ABI-shaped declarations only. This does not emulate filesystem durability. */
#ifndef PFR_TEST_SYS_FILE_H
#define PFR_TEST_SYS_FILE_H
#include <stdint.h>
typedef struct { uint64_t st_size; } sysFSStat;
int32_t sysLv2FsLSeek64(int32_t,uint64_t,int32_t,uint64_t *);
int32_t sysLv2FsRead(int32_t,void *,uint64_t,uint64_t *);
int32_t sysLv2FsWrite(int32_t,const void *,uint64_t,uint64_t *);
int32_t sysLv2FsFStat(int32_t,sysFSStat *);
int32_t sysLv2FsFtruncate(int32_t,uint64_t);
int32_t sysLv2FsFsync(int32_t);
#endif
