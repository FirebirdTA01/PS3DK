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

#endif /* __SYS_PRX_H__ */
