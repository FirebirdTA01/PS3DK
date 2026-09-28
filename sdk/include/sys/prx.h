/* Legacy PRX types.  Includes <lv2/prx.h> for the calls, and each header
 * has its own guard, so either can be included first.
 *
 * The user-pointer fields of the module list and module info records are
 * 32-bit effective addresses: real pointers on ILP32, u32 on LP64. */
#ifndef __SYS_PRX_H__
#define __SYS_PRX_H__

#include <stddef.h>
#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_PRX_RESIDENT                         0
#define SYS_PRX_NO_RESIDENT                      1

#define SYS_PRX_START_OK                         0

#define SYS_PRX_STOP_SUCCESS                     0
#define SYS_PRX_STOP_OK                          0
#define SYS_PRX_STOP_FAIL                        1

#define SYS_PRX_MODULE_FILENAME_SIZE             512

#define SYS_PRX_PROCESS_ELF_ID                   0

#define SYS_PRX_LOAD_MODULE_FLAGS_VALIDMASK     0x0000000000000001
#define SYS_PRX_LOAD_MODULE_FLAGS_FIXEDADDR     0x0000000000000001

typedef s32 sysPrxId;
typedef u64 sysPrxFlags;

typedef struct _sys_prx_segment_info {
    u64 base;
    u64 filesize;
    u64 memsize;
    u64 index;
    u64 type;
} sysPrxSegmentInfo;

typedef s32 (*sys_prx_entry_t)(size_t args, void *argv);
typedef s32 (*sys_prx_entry_pe_t)(u64 entry, size_t args, void *argv);

typedef struct _sys_prx_start_option {
    u64 size;
} sysPrxStartOption;

typedef struct _sys_prx_stop_option {
    u64 size;
} sysPrxStopOption;

typedef struct _sys_prx_load_module_option {
    u64 size;
} sysPrxLoadModuleOption;

typedef struct _sys_prx_load_module_list_option {
    u64 size;
} sysPrxLoadModuleListOption;

typedef struct _sys_prx_start_module_option {
    u64 size;
} sysPrxStartModuleOption;

typedef struct _sys_prx_stop_module_option {
    u64 size;
} sysPrxStopModuleOption;

typedef struct _sys_prx_unload_module_option {
    u64 size;
} sysPrxUnloadModuleOption;

typedef struct _sys_prx_register_module_option {
    u64 size;
} sysPrxRegisterModuleOption;

typedef struct _sys_prx_get_module_id_by_name_option {
    u64 size;
} sysPrxGetModuleIdByNameOption;

#ifdef __LP64__
typedef u32 sysPrxUserPchar;
typedef u32 sysPrxUserSegmentVector;
typedef u32 sysPrxUserPprxId;
typedef u32 sysPrxUserPconstVoid;
typedef u32 sysPrxUserPstopLevel;
#else
typedef char *sysPrxUserPchar;
typedef sysPrxSegmentInfo *sysPrxUserSegmentVector;
typedef sysPrxId *sysPrxUserPprxId;
typedef const void *sysPrxUserPconstVoid;
typedef const void *sysPrxUserPstopLevel;
#endif
/* Earlier spelling of sysPrxUserPchar, kept for existing code. */
typedef sysPrxUserPchar sysPrxUser_pchar;

typedef struct sys_prx_get_module_list_t {
    u64 size;
    u32 max;
    u32 count;
    sysPrxUserPprxId idlist;
    sysPrxUserPstopLevel levellist;
} sysPrxModuleList;

typedef struct sys_prx_module_info_t {
    u64 size;
    char name[30];
    char version[2];
    u32 modattribute;
    u32 start_entry;
    u32 stop_entry;
    u32 all_segments_num;
    sysPrxUserPchar filename;
    u32 filename_size;
    sysPrxUserSegmentVector segments;
    u32 segments_num;
} sysPrxModuleInfo;

#ifdef __cplusplus
}
#endif

#include <lv2/prx.h>

/* Cell names.  The types are the structures above; the calls forward to
 * the sysPrxForUser entry points, which the lv2 stub library exports under
 * the older names. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef sysPrxId sys_prx_id_t;
typedef sysPrxFlags sys_prx_flags_t;
typedef sysPrxSegmentInfo sys_prx_segment_info_t;
typedef sysPrxStartOption sys_prx_start_option_t;
typedef sysPrxStopOption sys_prx_stop_option_t;
typedef sysPrxLoadModuleOption sys_prx_load_module_option_t;
typedef sysPrxLoadModuleListOption sys_prx_load_module_list_option_t;
typedef sysPrxStartModuleOption sys_prx_start_module_option_t;
typedef sysPrxStopModuleOption sys_prx_stop_module_option_t;
typedef sysPrxUnloadModuleOption sys_prx_unload_module_option_t;
typedef sysPrxRegisterModuleOption sys_prx_register_module_option_t;
typedef sysPrxGetModuleIdByNameOption sys_prx_get_module_id_by_name_option_t;
typedef sysPrxModuleList sys_prx_get_module_list_t;
typedef sysPrxModuleInfo sys_prx_module_info_t;
typedef sysPrxUserPchar sys_prx_user_pchar_t;
typedef sysPrxUserSegmentVector sys_prx_user_segment_vector_t;
typedef sysPrxUserPprxId sys_prx_user_p_prx_id_t;
typedef sysPrxUserPconstVoid sys_prx_user_p_const_void_t;
typedef sysPrxUserPstopLevel sys_prx_user_p_stop_level_t;

#define SYS_PRX_STOP_FAILED SYS_PRX_STOP_FAIL

/* entry points without an older declaration */
sysPrxId sysPrxLoadModuleByFd(int fd, int64_t offset, sysPrxFlags flags, sysPrxLoadModuleOption *opt);
sysPrxId sysPrxLoadModuleOnMemcontainer(const char *path, uint32_t container, sysPrxFlags flags,
                                        sysPrxLoadModuleOption *opt);
sysPrxId sysPrxLoadModuleOnMemcontainerByFd(int fd, int64_t offset, uint32_t container, sysPrxFlags flags,
                                            sysPrxLoadModuleOption *opt);
s32 sysPrxLoadModuleList(int n, const char **pathList, sysPrxFlags flags, sysPrxLoadModuleListOption *opt,
                         sysPrxId *idList);
s32 sysPrxLoadModuleListOnMemcontainer(int n, const char **pathList, uint32_t container, sysPrxFlags flags,
                                       sysPrxLoadModuleListOption *opt, sysPrxId *idList);
sysPrxId sysPrxGetModuleIdByAddress(void *addr);
sysPrxId sysPrxGetModuleId(void);

static inline sys_prx_id_t sys_prx_load_module(const char *path, uint64_t flags, sys_prx_load_module_option_t *opt)
{ return sysPrxLoadModule(path, flags, opt); }
static inline sys_prx_id_t sys_prx_load_module_by_fd(int fd, int64_t offset, uint64_t flags,
                                                     sys_prx_load_module_option_t *opt)
{ return sysPrxLoadModuleByFd(fd, offset, flags, opt); }
static inline sys_prx_id_t sys_prx_load_module_on_memcontainer(const char *path, uint32_t container, uint64_t flags,
                                                               sys_prx_load_module_option_t *opt)
{ return sysPrxLoadModuleOnMemcontainer(path, container, flags, opt); }
static inline sys_prx_id_t sys_prx_load_module_on_memcontainer_by_fd(int fd, int64_t offset, uint32_t container,
                                                                     uint64_t flags,
                                                                     sys_prx_load_module_option_t *opt)
{ return sysPrxLoadModuleOnMemcontainerByFd(fd, offset, container, flags, opt); }
static inline int sys_prx_load_module_list(int n, const char **pathList, uint64_t flags,
                                           sys_prx_load_module_list_option_t *opt, sys_prx_id_t *idList)
{ return sysPrxLoadModuleList(n, pathList, flags, opt, idList); }
static inline int sys_prx_load_module_list_on_memcontainer(int n, const char **pathList, uint32_t container,
                                                           uint64_t flags, sys_prx_load_module_list_option_t *opt,
                                                           sys_prx_id_t *idList)
{ return sysPrxLoadModuleListOnMemcontainer(n, pathList, container, flags, opt, idList); }
static inline int sys_prx_start_module(sys_prx_id_t id, size_t args, void *argp, int *modres, sys_prx_flags_t flags,
                                       sys_prx_start_module_option_t *opt)
{ return sysPrxStartModule(id, args, argp, modres, flags, opt); }
static inline int sys_prx_stop_module(sys_prx_id_t id, size_t args, void *argp, int *modres, sys_prx_flags_t flags,
                                      sys_prx_stop_module_option_t *opt)
{ return sysPrxStopModule(id, args, argp, modres, flags, (sysPrxStartModuleOption *)opt); }
static inline int sys_prx_unload_module(sys_prx_id_t id, sys_prx_flags_t flags,
                                        const sys_prx_unload_module_option_t *opt)
{ return sysPrxUnloadModule(id, flags, (sysPrxLoadModuleOption *)(uintptr_t)opt); }
static inline int sys_prx_get_module_list(sys_prx_flags_t flags, sys_prx_get_module_list_t *info)
{ return sysPrxGetModuleList(flags, info); }
static inline int sys_prx_get_module_info(sys_prx_id_t id, sys_prx_flags_t flags, sys_prx_module_info_t *info)
{ return sysPrxGetModuleInfo(id, flags, info); }
static inline sys_prx_id_t sys_prx_get_module_id_by_name(const char *name, sys_prx_flags_t flags,
                                                         sys_prx_get_module_id_by_name_option_t *opt)
{ return sysPrxGetModuleIdByName(name, flags, opt); }
static inline sys_prx_id_t sys_prx_get_module_id_by_address(void *addr)
{ return sysPrxGetModuleIdByAddress(addr); }
static inline sys_prx_id_t sys_prx_get_my_module_id(void)
{ return sysPrxGetModuleId(); }

#ifdef __cplusplus
}
#endif

#endif /* __SYS_PRX_H__ */
