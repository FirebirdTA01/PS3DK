/* tests/sdk/fixtures/spu-arg-fixture/sys/spu.h
 *
 * Test fixture for host-based verification of SPU thread argument handling.
 * Provides mock PSL1GHT types and doubles for PSL1GHT sysSpu* functions.
 */
#ifndef __TEST_FIXTURE_SYS_SPU_H__
#define __TEST_FIXTURE_SYS_SPU_H__

#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sysSpuImage {
    uint32_t           type;
    uint32_t           entry_point;
    void              *segs;
    int                nsegs;
} sysSpuImage;

typedef struct _sys_spu_thread_arg {
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
} sysSpuThreadArgument;

typedef struct _sys_spu_thread_attr {
    const char *name;
    uint32_t    nsize;
    uint32_t    option;
} sysSpuThreadAttribute;

typedef struct _sys_spu_thread_group_attr {
    uint32_t    nsize;
    const char *name;
    uint32_t    type;
    union {
        sys_mem_container_t ct;
    } option;
} sysSpuThreadGroupAttribute;

/* Recording double for sysSpuThreadInitialize */
typedef struct spu_thread_init_record {
    sys_spu_thread_t      *thread;
    sys_spu_group_t        group;
    uint32_t               spu;
    sysSpuImage           *image;
    sysSpuThreadAttribute *attributes;
    sysSpuThreadArgument   arguments;
    int                    call_count;
} spu_thread_init_record_t;

extern spu_thread_init_record_t g_spu_init_record;
extern int g_spu_init_ret_val;

static inline int sysSpuThreadInitialize(sys_spu_thread_t *thread,
                                         sys_spu_group_t group,
                                         uint32_t spu,
                                         sysSpuImage *image,
                                         sysSpuThreadAttribute *attributes,
                                         sysSpuThreadArgument *arguments)
{
    g_spu_init_record.thread = thread;
    g_spu_init_record.group = group;
    g_spu_init_record.spu = spu;
    g_spu_init_record.image = image;
    g_spu_init_record.attributes = attributes;
    if (arguments) {
        memcpy(&g_spu_init_record.arguments, arguments, sizeof(g_spu_init_record.arguments));
    }
    g_spu_init_record.call_count++;
    return g_spu_init_ret_val;
}

/* Host test doubles for other sysSpu* functions referenced by inline forwarders */
static inline int sysSpuImageImport(sysSpuImage *img, const void *src, uint32_t type) { (void)img; (void)src; (void)type; return 0; }
static inline int sysSpuImageClose(sysSpuImage *img) { (void)img; return 0; }
static inline int sysSpuImageOpen(sysSpuImage *img, const char *path) { (void)img; (void)path; return 0; }
static inline int sysSpuImageOpenFd(sysSpuImage *img, int fd, uint64_t offset) { (void)img; (void)fd; (void)offset; return 0; }

static inline int sysSpuThreadGroupCreate(sys_spu_group_t *id, unsigned int num, unsigned int prio, sysSpuThreadGroupAttribute *attr) { (void)id; (void)num; (void)prio; (void)attr; return 0; }
static inline int sysSpuThreadGroupStart(sys_spu_group_t id) { (void)id; return 0; }
static inline int sysSpuThreadGroupJoin(sys_spu_group_t gid, u32 *cause, u32 *status) { (void)gid; (void)cause; (void)status; return 0; }
static inline int sysSpuThreadGroupDestroy(sys_spu_group_t id) { (void)id; return 0; }
static inline int sysSpuThreadGroupTerminate(sys_spu_group_t id, u32 value) { (void)id; (void)value; return 0; }

static inline int sysSpuThreadGetExitStatus(sys_spu_thread_t id, s32 *status) { (void)id; (void)status; return 0; }
static inline int sysSpuThreadWriteSignal(sys_spu_thread_t id, u32 number, u32 value) { (void)id; (void)number; (void)value; return 0; }

#ifdef __cplusplus
}
#endif

#endif /* __TEST_FIXTURE_SYS_SPU_H__ */
