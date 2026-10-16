/* tests/sdk/sys-prx-records-test.c: the PRX record types and constants <sys/prx.h> carries, and the event
 * queue types of <sys/event.h>, with the values and layouts the system uses.  The records describe a
 * PRX's 32-bit export/import tables, so every size and offset is the same under ILP32 and LP64. */
#include <stddef.h>
#include <sys/prx.h>
#include <sys/event.h>

#if defined(__cplusplus)
# define CHECK(c, m) static_assert(c, m)
#else
# define CHECK(c, m) _Static_assert(c, m)
#endif

CHECK(SYS_PRX_LIB_OPD_IMPORT == 0x2000, "SYS_PRX_LIB_OPD_IMPORT");
CHECK(SYS_PRX_R_PPC_ADDR32 == 1 && SYS_PRX_R_PPC_ADDR16_LO == 4 && SYS_PRX_R_PPC_ADDR16_HI == 5 &&
      SYS_PRX_R_PPC_ADDR16_HA == 6, "PPC relocation types");
CHECK(SYS_PRX_R_PPC64_ADDR32 == 1 && SYS_PRX_R_PPC64_ADDR16_LO == 4 && SYS_PRX_R_PPC64_ADDR16_HI == 5 &&
      SYS_PRX_R_PPC64_ADDR16_HA == 6 && SYS_PRX_R_PPC64_ADDR64 == 38, "PPC64 relocation types");
CHECK(SYS_PRX_VARLINK_TERMINATE32 == 0, "SYS_PRX_VARLINK_TERMINATE32");

CHECK(sizeof(sys_prx_link32_t) == 12, "sys_prx_link32_t size");
CHECK(sizeof(sys_prx_libent32_t) == 28, "sys_prx_libent32_t size");
CHECK(offsetof(sys_prx_libent32_t, nidaltsets) == 15 && offsetof(sys_prx_libent32_t, libname) == 16 &&
      offsetof(sys_prx_libent32_t, addtable) == 24, "sys_prx_libent32_t layout");
CHECK(sizeof(sys_prx_libstub32_t) == 44, "sys_prx_libstub32_t size");
CHECK(offsetof(sys_prx_libstub32_t, libname) == 16 && offsetof(sys_prx_libstub32_t, tls_table) == 40,
      "sys_prx_libstub32_t layout");
CHECK(sizeof(sys_prx_user_libent_addr_t) == 4 && sizeof(sys_prx_user_libstub_addr_t) == 4,
      "user library addresses are 32-bit");
CHECK(sizeof(sys_prx_module_info_v2_t) == 88, "sys_prx_module_info_v2_t size");
CHECK(offsetof(sys_prx_module_info_v2_t, modattribute) == 40 && offsetof(sys_prx_module_info_v2_t, filename) == 56 &&
      offsetof(sys_prx_module_info_v2_t, segments_num) == 68 && offsetof(sys_prx_module_info_v2_t, libent_addr) == 72 &&
      offsetof(sys_prx_module_info_v2_t, libstub_addr) == 80 && offsetof(sys_prx_module_info_v2_t, libstub_size) == 84,
      "sys_prx_module_info_v2_t layout");
/* v2 extends the v1 record: the shared prefix lines up. */
CHECK(offsetof(sys_prx_module_info_v2_t, segments_num) == offsetof(sys_prx_module_info_t, segments_num),
      "v2 prefix matches v1");

CHECK(SYS_PPU_QUEUE == 1 && SYS_SPU_QUEUE == 2, "event queue types");

int sys_prx_records_probe(sys_prx_module_info_v2_t *info, sys_event_queue_attribute_t *attr)
{
    attr->type = SYS_SPU_QUEUE;
    info->size = sizeof *info;
    return (int)info->libent_size + (int)info->libstub_size;
}
