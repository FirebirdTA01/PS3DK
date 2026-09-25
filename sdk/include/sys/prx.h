/*
 * PS3 Custom Toolchain — <sys/prx.h>
 *
 * CellOS Lv-2 PRX (relocatable module / dynamic library) definitions.
 *
 * Provides canonical sys_prx_* types and functions, PSL1GHT sysPrx*
 * backward-compatibility aliases, anonymous unions in sys_prx_segment_info_t
 * for filesz/filesize and memsz/memsize, and module authoring macros
 * (SYS_MODULE_INFO, SYS_MODULE_START, etc.).
 */

#ifndef __SYS_PRX_H__
#define __SYS_PRX_H__

#ifndef __LV2_PRX_H__
#define __LV2_PRX_H__
#endif

#include <ppu-types.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/sys_types.h>

#ifndef _OFF64_T_DEFINED
#define _OFF64_T_DEFINED
typedef int64_t off64_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Return values of module_start / module_stop */
#define SYS_PRX_RESIDENT                         0   /* stay resident */
#define SYS_PRX_NO_RESIDENT                      1   /* unload immediately */

#define SYS_PRX_START_OK                         SYS_PRX_RESIDENT

#define SYS_PRX_STOP_SUCCESS                     0
#define SYS_PRX_STOP_OK                          SYS_PRX_STOP_SUCCESS
#define SYS_PRX_STOP_FAILED                      1
#define SYS_PRX_STOP_FAIL                        SYS_PRX_STOP_FAILED

#define SYS_PRX_MODULE_FILENAME_SIZE             512

#define SYS_PRX_PROCESS_ELF_ID                   0

#define SYS_PRX_LOAD_MODULE_FLAGS_VALIDMASK     0x0000000000000001ULL
#define SYS_PRX_LOAD_MODULE_FLAGS_FIXEDADDR     0x0000000000000001ULL

#define SYS_PRX_LIB_OPD_IMPORT                   0x2000

/* PowerPC relocations defined by the ABIs */
#define SYS_PRX_R_PPC_ADDR32                     1
#define SYS_PRX_R_PPC_ADDR16_LO                  4
#define SYS_PRX_R_PPC_ADDR16_HI                  5
#define SYS_PRX_R_PPC_ADDR16_HA                  6

#define SYS_PRX_R_PPC64_ADDR32                   SYS_PRX_R_PPC_ADDR32
#define SYS_PRX_R_PPC64_ADDR16_LO                SYS_PRX_R_PPC_ADDR16_LO
#define SYS_PRX_R_PPC64_ADDR16_HI                SYS_PRX_R_PPC_ADDR16_HI
#define SYS_PRX_R_PPC64_ADDR16_HA                SYS_PRX_R_PPC_ADDR16_HA
#define SYS_PRX_R_PPC64_ADDR64                   38
#define SYS_PRX_VARLINK_TERMINATE32              0x00000000

/* Module attributes and levels */
#define SYS_MODULE_NAME_LEN                      27
#define SYS_MODULE_MAX_SEGMENTS                  4

#define SYS_MODULE_ATTR_CANT_STOP                (0x0001)
#define SYS_MODULE_ATTR_EXCLUSIVE_LOAD           (0x0002)
#define SYS_MODULE_ATTR_EXCLUSIVE_START          (0x0004)

#define SYS_MODULE_STOP_LEVEL_USER               0x00000000
#define SYS_MODULE_STOP_LEVEL_SYSTEM             0x00004000

/* Module authoring macros */
#define SYS_MODULE_INFO(name, attr, major, minor) \
    static const char __sys_module_info_##name[] __attribute__((unused)) = #name

#define SYS_MODULE_START(funcname) \
    __asm__(".pushsection .rodata.sceModuleStart,\"a\"\n" \
            ".quad " #funcname "\n" \
            ".popsection\n")

#define SYS_MODULE_STOP(funcname) \
    __asm__(".pushsection .rodata.sceModuleStop,\"a\"\n" \
            ".quad " #funcname "\n" \
            ".popsection\n")

#define SYS_MODULE_EXIT(funcname) \
    __asm__(".pushsection .rodata.sceModuleExit,\"a\"\n" \
            ".quad " #funcname "\n" \
            ".popsection\n")

#define SYS_MODULE_PROLOGUE(funcname) \
    __asm__(".pushsection .rodata.sceModulePrologue,\"a\"\n" \
            ".quad " #funcname "\n" \
            ".popsection\n")

#define SYS_MODULE_EPILOGUE(funcname) \
    __asm__(".pushsection .rodata.sceModuleEpilogue,\"a\"\n" \
            ".quad " #funcname "\n" \
            ".popsection\n")

#define SYS_MODULE_REBOOT_BEFORE(funcname) \
    __asm__(".pushsection .rodata.sceModuleRebootBefore,\"a\"\n" \
            ".quad " #funcname "\n" \
            ".popsection\n")

#define SYS_MODULE_STOP_LEVEL(level) \
    static const int __sys_module_stop_level __attribute__((unused)) = (level)

#define SYS_MODULE_START_THREAD_PARAMETER(initPriority, stackSize, attr) \
    static const int __sys_module_startthpara[] __attribute__((unused)) = {3, (initPriority), (stackSize), (attr)}

#define SYS_MODULE_STOP_THREAD_PARAMETER(initPriority, stackSize, attr) \
    static const int __sys_module_stopthpara[] __attribute__((unused)) = {3, (initPriority), (stackSize), (attr)}

/* Canonical and PSL1GHT basic types */
typedef int32_t sys_prx_id_t;
typedef sys_prx_id_t sysPrxId;

typedef uint64_t sys_prx_flags_t;
typedef sys_prx_flags_t sysPrxFlags;

/*
 * Segment information structure.
 * Uses anonymous unions to support both canonical (filesz, memsz)
 * and PSL1GHT (filesize, memsize) member names without macros.
 */
typedef struct _sys_prx_segment_info {
    uint64_t base;
    union {
        uint64_t filesz;
        uint64_t filesize;
    };
    union {
        uint64_t memsz;
        uint64_t memsize;
    };
    uint64_t index;
    uint64_t type;
} sys_prx_segment_info_t, sysPrxSegmentInfo;

typedef struct sys_prx_link32_t {
    uint32_t type;
    uint32_t address;
    uint32_t addend;
} sys_prx_link32_t;

typedef struct sys_prx_libent32_t {
    unsigned char structsize;
    unsigned char reserved1[1];
    unsigned short version;
    unsigned short attribute;
    unsigned short nfunc;
    unsigned short nvar;
    unsigned short ntls;
    unsigned char hashinfo;
    unsigned char hashinfo2;
    unsigned char reserved2[1];
    unsigned char nidaltsets;
    uint32_t libname;
    uint32_t nidtable;
    uint32_t addtable;
} sys_prx_libent32_t;

typedef struct sys_prx_libstub32_t {
    unsigned char structsize;
    unsigned char reserved1[1];
    unsigned short version;
    unsigned short attribute;
    unsigned short nfunc;
    unsigned short nvar;
    unsigned short ntls;
    unsigned char reserved2[4];
    uint32_t libname;
    uint32_t func_nidtable;
    uint32_t func_table;
    uint32_t var_nidtable;
    uint32_t var_table;
    uint32_t tls_nidtable;
    uint32_t tls_table;
} sys_prx_libstub32_t;

typedef int (*sys_prx_entry_t)(size_t args, void *argv);
typedef int (*sys_prx_entry_pe_t)(uint64_t entry, size_t args, void *argv);

/* Option structures */
typedef struct sys_prx_start_option_t {
    uint64_t size;
} sys_prx_start_option_t, sysPrxStartOption;

typedef struct sys_prx_stop_option_t {
    uint64_t size;
} sys_prx_stop_option_t, sysPrxStopOption;

typedef struct sys_prx_load_module_option_t {
    uint64_t size;
} sys_prx_load_module_option_t, sysPrxLoadModuleOption;

typedef struct sys_prx_load_module_list_option_t {
    uint64_t size;
} sys_prx_load_module_list_option_t, sysPrxLoadModuleListOption;

typedef struct sys_prx_start_module_option_t {
    uint64_t size;
} sys_prx_start_module_option_t, sysPrxStartModuleOption;

typedef struct sys_prx_stop_module_option_t {
    uint64_t size;
} sys_prx_stop_module_option_t, sysPrxStopModuleOption;

typedef struct sys_prx_unload_module_option_t {
    uint64_t size;
} sys_prx_unload_module_option_t, sysPrxUnloadModuleOption;

typedef struct sys_prx_register_module_option_t {
    uint64_t size;
} sys_prx_register_module_option_t, sysPrxRegisterModuleOption;

typedef struct sys_prx_get_module_id_by_name_option_t {
    uint64_t size;
} sys_prx_get_module_id_by_name_option_t, sysPrxGetModuleIdByNameOption;

#ifdef __LP64__
typedef uint32_t sys_prx_user_pchar_t;
typedef uint32_t sysPrxUserPchar;
typedef uint32_t sysPrxUser_pchar;

typedef uint32_t sys_prx_user_segment_vector_t;
typedef uint32_t sysPrxUserSegmentVector;

typedef uint32_t sys_prx_user_libent_addr_t;
typedef uint32_t sys_prx_user_libstub_addr_t;

typedef uint32_t sys_prx_user_p_prx_id_t;
typedef uint32_t sysPrxUserPprxId;

typedef uint32_t sys_prx_user_p_const_void_t;
typedef uint32_t sysPrxUserPconstVoid;

typedef uint32_t sys_prx_user_p_stop_level_t;
typedef uint32_t sysPrxUserPstopLevel;
#else
typedef char *sys_prx_user_pchar_t;
typedef char *sysPrxUserPchar;
typedef char *sysPrxUser_pchar;

typedef sys_prx_segment_info_t *sys_prx_user_segment_vector_t;
typedef sys_prx_segment_info_t *sysPrxUserSegmentVector;

typedef sys_prx_libent32_t *sys_prx_user_libent_addr_t;
typedef sys_prx_libstub32_t *sys_prx_user_libstub_addr_t;

typedef sys_prx_id_t *sys_prx_user_p_prx_id_t;
typedef sys_prx_id_t *sysPrxUserPprxId;

typedef const void *sys_prx_user_p_const_void_t;
typedef const void *sysPrxUserPconstVoid;

typedef const void *sys_prx_user_p_stop_level_t;
typedef const void *sysPrxUserPstopLevel;
#endif

typedef struct sys_prx_get_module_list_t {
    uint64_t size;
    uint32_t max;
    uint32_t count;
    sys_prx_user_p_prx_id_t idlist;
    sys_prx_user_p_stop_level_t levellist;
} sys_prx_get_module_list_t, sysPrxModuleList;

typedef struct sys_prx_module_info_t {
    uint64_t size;
    char name[30];
    char version[2];
    uint32_t modattribute;
    uint32_t start_entry;
    uint32_t stop_entry;
    uint32_t all_segments_num;
    sys_prx_user_pchar_t filename;
    uint32_t filename_size;
    sys_prx_user_segment_vector_t segments;
    uint32_t segments_num;
} sys_prx_module_info_t, sysPrxModuleInfo;

typedef struct sys_prx_module_info_v2_t {
    uint64_t size;
    char name[30];
    char version[2];
    uint32_t modattribute;
    uint32_t start_entry;
    uint32_t stop_entry;
    uint32_t all_segments_num;
    sys_prx_user_pchar_t filename;
    uint32_t filename_size;
    sys_prx_user_segment_vector_t segments;
    uint32_t segments_num;
    sys_prx_user_libent_addr_t libent_addr;
    uint32_t libent_size;
    sys_prx_user_libstub_addr_t libstub_addr;
    uint32_t libstub_size;
} sys_prx_module_info_v2_t;

/* Shared library interface */
extern int sys_prx_version;
#define sysPrxVersion sys_prx_version

sys_prx_id_t sys_prx_load_module(const char *path, uint64_t flags,
                                 sys_prx_load_module_option_t *pOpt);
sys_prx_id_t sys_prx_load_module_on_memcontainer(const char *path,
                                                 sys_memory_container_t mem_container,
                                                 uint64_t flags,
                                                 sys_prx_load_module_option_t *pOpt);
sys_prx_id_t sys_prx_load_module_by_fd(int fd, off64_t offset, uint64_t flags,
                                       sys_prx_load_module_option_t *pOpt);
sys_prx_id_t sys_prx_load_module_on_memcontainer_by_fd(int fd, off64_t offset,
                                                       sys_memory_container_t mem_container,
                                                       uint64_t flags,
                                                       sys_prx_load_module_option_t *pOpt);

int sys_prx_load_module_list(int n, const char **path_list, uint64_t flags,
                             sys_prx_load_module_list_option_t *pOpt,
                             sys_prx_id_t *idlist);
int sys_prx_load_module_list_on_memcontainer(int n, const char **path_list,
                                             sys_memory_container_t mem_container,
                                             uint64_t flags,
                                             sys_prx_load_module_list_option_t *pOpt,
                                             sys_prx_id_t *idlist);

int sys_prx_start_module(sys_prx_id_t id, size_t args, void *argp,
                         int *modres, sys_prx_flags_t flags,
                         sys_prx_start_module_option_t *pOpt);
int sys_prx_stop_module(sys_prx_id_t id, size_t args, void *argp,
                        int *modres, sys_prx_flags_t flags,
                        sys_prx_stop_module_option_t *pOpt);
int sys_prx_unload_module(sys_prx_id_t id, sys_prx_flags_t flags,
                          const sys_prx_unload_module_option_t *pOpt);

int sys_prx_register_module(const sys_prx_register_module_option_t *pOpt);

int sys_prx_get_module_list(sys_prx_flags_t flags,
                            sys_prx_get_module_list_t *pInfo);
int sys_prx_get_module_info(sys_prx_id_t id, sys_prx_flags_t flags,
                            sys_prx_module_info_t *p_info);
sys_prx_id_t sys_prx_get_module_id_by_name(const char *name,
                                           sys_prx_flags_t flags,
                                           sys_prx_get_module_id_by_name_option_t *pOpt);
sys_prx_id_t sys_prx_get_module_id_by_address(void *addr);
sys_prx_id_t sys_prx_get_my_module_id(void);
sys_addr_t sys_prx_get_ppu_guid(sys_prx_id_t modid);
int sys_prx_register_library(void *pLibEnt);
int sys_prx_unregister_library(void *pLibEnt);
int sys_prx_exitspawn_with_level(sys_prx_id_t id, int level, uint64_t flags);

/* PSL1GHT compatibility function declarations */
sysPrxId sysPrxLoadModule(const char *path, sysPrxFlags flags, sysPrxLoadModuleOption *opt);
sysPrxId sysPrxUnloadModule(sysPrxId id, sysPrxFlags flags, sysPrxUnloadModuleOption *opt);
s32 sysPrxStartModule(sysPrxId id, size_t args, void *argp, s32 *modres, sysPrxFlags flags, sysPrxStartModuleOption *opt);
s32 sysPrxStopModule(sysPrxId id, size_t args, void *argp, s32 *modres, sysPrxFlags flags, sysPrxStopModuleOption *opt);
s32 sysPrxRegisterModule(const sysPrxRegisterModuleOption *opt);
s32 sysPrxGetModuleList(sysPrxFlags flags, sysPrxModuleList *list);
s32 sysPrxGetModuleInfo(sysPrxId id, sysPrxFlags flags, sysPrxModuleInfo *info);
sysPrxId sysPrxGetModuleIdByName(const char *name, sysPrxFlags flags, sysPrxGetModuleIdByNameOption *opt);
sysPrxId sysPrxGetModuleId(void);
sysPrxId sysPrxGetModuleIdByAddress(void *addr);

#ifdef __cplusplus
}
#endif

#endif /* __SYS_PRX_H__ */
