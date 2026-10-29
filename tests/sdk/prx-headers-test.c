/* Include <sys/prx.h> and <lv2/prx.h> in the order ORDER_* selects, then use
 * the types and the calls: both must be visible whichever header comes
 * first. */
#if defined(ORDER_SYS_FIRST)
#include <sys/prx.h>
#include <lv2/prx.h>
#elif defined(ORDER_LV2_FIRST)
#include <lv2/prx.h>
#include <sys/prx.h>
#elif defined(ORDER_SYS_ONLY)
#include <sys/prx.h>
#elif defined(ORDER_LV2_ONLY)
#include <lv2/prx.h>
#else
#error "define one ORDER_*"
#endif

#define CHECK(name, cond) typedef char name[(cond) ? 1 : -1]

/* The user-pointer fields are 32-bit effective addresses on both ABIs. */
CHECK(filename_is_32_bits, sizeof(((sysPrxModuleInfo *)0)->filename) == 4);
CHECK(segments_is_32_bits, sizeof(((sysPrxModuleInfo *)0)->segments) == 4);
CHECK(idlist_is_32_bits, sizeof(((sysPrxModuleList *)0)->idlist) == 4);
CHECK(old_spelling_is_same_type, sizeof(sysPrxUser_pchar) == sizeof(sysPrxUserPchar));

/* Segment sizes use the system spelling filesz/memsz; the older filesize/memsize
 * names alias the same storage, so the record layout does not change. */
CHECK(segment_info_is_40_bytes, sizeof(sys_prx_segment_info_t) == 40);
CHECK(filesz_aliases_filesize, __builtin_offsetof(sys_prx_segment_info_t, filesz) == __builtin_offsetof(sys_prx_segment_info_t, filesize));
CHECK(memsz_aliases_memsize, __builtin_offsetof(sys_prx_segment_info_t, memsz) == __builtin_offsetof(sys_prx_segment_info_t, memsize));
CHECK(filesz_is_64_bits, sizeof(((sys_prx_segment_info_t *)0)->filesz) == 8);

sysPrxId (*const load_module)(const char *, sysPrxFlags, sysPrxLoadModuleOption *) = sysPrxLoadModule;
s32 (*const get_module_info)(sysPrxId, sysPrxFlags, sysPrxModuleInfo *) = sysPrxGetModuleInfo;
s32 (*const get_module_list)(sysPrxFlags, sysPrxModuleList *) = sysPrxGetModuleList;

int prx_probe(void)
{
    sysPrxModuleInfo info;
    sysPrxUserPchar name = 0;
    info.size = sizeof info;
    info.filename = name;
    info.filename_size = SYS_PRX_MODULE_FILENAME_SIZE;
    return (int)info.filename_size;
}
