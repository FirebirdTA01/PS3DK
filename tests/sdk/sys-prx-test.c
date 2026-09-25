/*
 * sys-prx-test.c - Static assertion, member offset, aggregate initialization,
 * and type compatibility probe for <sys/prx.h>.
 *
 * Verifies:
 * 1. sizeof(sys_prx_segment_info_t) == 40 and sizeof(sysPrxSegmentInfo) == 40
 * 2. Exact member offsets (base=0, filesz/filesize=8, memsz/memsize=16, index=24, type=32)
 * 3. Positional aggregate initialization {base, filesz, memsz, index, type}
 * 4. All PSL1GHT sysPrx* types, structs, aliases, and function declarations compile
 * 5. All canonical sys_prx_* types, options, and function declarations compile
 * 6. Module authoring macros (SYS_MODULE_INFO, SYS_MODULE_START, etc.) expand cleanly
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/prx.h>

/* 1 & 2. Static assertions for sizeof and member offsets */
#if defined(__cplusplus)
static_assert(sizeof(sys_prx_segment_info_t) == 40, "sizeof(sys_prx_segment_info_t) must be 40");
static_assert(sizeof(sysPrxSegmentInfo) == 40, "sizeof(sysPrxSegmentInfo) must be 40");
static_assert(offsetof(sys_prx_segment_info_t, base) == 0, "offset of base must be 0");
static_assert(offsetof(sys_prx_segment_info_t, filesz) == 8, "offset of filesz must be 8");
static_assert(offsetof(sys_prx_segment_info_t, filesize) == 8, "offset of filesize must be 8");
static_assert(offsetof(sys_prx_segment_info_t, memsz) == 16, "offset of memsz must be 16");
static_assert(offsetof(sys_prx_segment_info_t, memsize) == 16, "offset of memsize must be 16");
static_assert(offsetof(sys_prx_segment_info_t, index) == 24, "offset of index must be 24");
static_assert(offsetof(sys_prx_segment_info_t, type) == 32, "offset of type must be 32");

static_assert(offsetof(sysPrxSegmentInfo, base) == 0, "offset of base must be 0");
static_assert(offsetof(sysPrxSegmentInfo, filesz) == 8, "offset of filesz must be 8");
static_assert(offsetof(sysPrxSegmentInfo, filesize) == 8, "offset of filesize must be 8");
static_assert(offsetof(sysPrxSegmentInfo, memsz) == 16, "offset of memsz must be 16");
static_assert(offsetof(sysPrxSegmentInfo, memsize) == 16, "offset of memsize must be 16");
static_assert(offsetof(sysPrxSegmentInfo, index) == 24, "offset of index must be 24");
static_assert(offsetof(sysPrxSegmentInfo, type) == 32, "offset of type must be 32");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(sys_prx_segment_info_t) == 40, "sizeof(sys_prx_segment_info_t) must be 40");
_Static_assert(sizeof(sysPrxSegmentInfo) == 40, "sizeof(sysPrxSegmentInfo) must be 40");
_Static_assert(offsetof(sys_prx_segment_info_t, base) == 0, "offset of base must be 0");
_Static_assert(offsetof(sys_prx_segment_info_t, filesz) == 8, "offset of filesz must be 8");
_Static_assert(offsetof(sys_prx_segment_info_t, filesize) == 8, "offset of filesize must be 8");
_Static_assert(offsetof(sys_prx_segment_info_t, memsz) == 16, "offset of memsz must be 16");
_Static_assert(offsetof(sys_prx_segment_info_t, memsize) == 16, "offset of memsize must be 16");
_Static_assert(offsetof(sys_prx_segment_info_t, index) == 24, "offset of index must be 24");
_Static_assert(offsetof(sys_prx_segment_info_t, type) == 32, "offset of type must be 32");

_Static_assert(offsetof(sysPrxSegmentInfo, base) == 0, "offset of base must be 0");
_Static_assert(offsetof(sysPrxSegmentInfo, filesz) == 8, "offset of filesz must be 8");
_Static_assert(offsetof(sysPrxSegmentInfo, filesize) == 8, "offset of filesize must be 8");
_Static_assert(offsetof(sysPrxSegmentInfo, memsz) == 16, "offset of memsz must be 16");
_Static_assert(offsetof(sysPrxSegmentInfo, memsize) == 16, "offset of memsize must be 16");
_Static_assert(offsetof(sysPrxSegmentInfo, index) == 24, "offset of index must be 24");
_Static_assert(offsetof(sysPrxSegmentInfo, type) == 32, "offset of type must be 32");
#else
typedef char assert_sizeof_prx_seg[(sizeof(sys_prx_segment_info_t) == 40) ? 1 : -1];
typedef char assert_sizeof_psl_seg[(sizeof(sysPrxSegmentInfo) == 40) ? 1 : -1];
typedef char assert_offset_base[(offsetof(sys_prx_segment_info_t, base) == 0) ? 1 : -1];
typedef char assert_offset_filesz[(offsetof(sys_prx_segment_info_t, filesz) == 8) ? 1 : -1];
typedef char assert_offset_filesize[(offsetof(sys_prx_segment_info_t, filesize) == 8) ? 1 : -1];
typedef char assert_offset_memsz[(offsetof(sys_prx_segment_info_t, memsz) == 16) ? 1 : -1];
typedef char assert_offset_memsize[(offsetof(sys_prx_segment_info_t, memsize) == 16) ? 1 : -1];
typedef char assert_offset_index[(offsetof(sys_prx_segment_info_t, index) == 24) ? 1 : -1];
typedef char assert_offset_type[(offsetof(sys_prx_segment_info_t, type) == 32) ? 1 : -1];
#endif

/* 3. Positional aggregate initialization */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-braces"
#endif
static int test_aggregate_init(void)
{
    sys_prx_segment_info_t seg = { 0x1000ULL, 0x2000ULL, 0x3000ULL, 4ULL, 5ULL };
    if (seg.base != 0x1000ULL) return 1;
    if (seg.filesz != 0x2000ULL) return 2;
    if (seg.filesize != 0x2000ULL) return 3;
    if (seg.memsz != 0x3000ULL) return 4;
    if (seg.memsize != 0x3000ULL) return 5;
    if (seg.index != 4ULL) return 6;
    if (seg.type != 5ULL) return 7;

    sysPrxSegmentInfo pseg = { 0x5000ULL, 0x6000ULL, 0x7000ULL, 8ULL, 9ULL };
    if (pseg.base != 0x5000ULL) return 8;
    if (pseg.filesz != 0x6000ULL) return 9;
    if (pseg.filesize != 0x6000ULL) return 10;
    if (pseg.memsz != 0x7000ULL) return 11;
    if (pseg.memsize != 0x7000ULL) return 12;
    if (pseg.index != 8ULL) return 13;
    if (pseg.type != 9ULL) return 14;

    return 0;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* 4 & 5. Compile check for all canonical and PSL1GHT types & declarations */
static int test_compat_types_and_decls(void)
{
    sysPrxId pid = 1;
    sys_prx_id_t cid = pid;
    (void)cid;

    sysPrxFlags pflags = 0;
    sys_prx_flags_t cflags = pflags;
    (void)cflags;

    sysPrxStartOption pso;
    pso.size = sizeof(pso);
    sys_prx_start_option_t cso;
    cso.size = sizeof(cso);

    sysPrxStopOption psto;
    psto.size = sizeof(psto);
    sys_prx_stop_option_t csto;
    csto.size = sizeof(csto);

    sysPrxLoadModuleOption plmo;
    plmo.size = sizeof(plmo);
    sys_prx_load_module_option_t clmo;
    clmo.size = sizeof(clmo);

    sysPrxLoadModuleListOption plmlo;
    plmlo.size = sizeof(plmlo);
    sys_prx_load_module_list_option_t clmlo;
    clmlo.size = sizeof(clmlo);

    sysPrxStartModuleOption psmo;
    psmo.size = sizeof(psmo);
    sys_prx_start_module_option_t csmo;
    csmo.size = sizeof(csmo);

    sysPrxStopModuleOption pstmo;
    pstmo.size = sizeof(pstmo);
    sys_prx_stop_module_option_t cstmo;
    cstmo.size = sizeof(cstmo);

    sysPrxUnloadModuleOption pulmo;
    pulmo.size = sizeof(pulmo);
    sys_prx_unload_module_option_t culmo;
    culmo.size = sizeof(culmo);

    sysPrxRegisterModuleOption prmo;
    prmo.size = sizeof(prmo);
    sys_prx_register_module_option_t crmo;
    crmo.size = sizeof(crmo);

    sysPrxGetModuleIdByNameOption pgno;
    pgno.size = sizeof(pgno);
    sys_prx_get_module_id_by_name_option_t cgno;
    cgno.size = sizeof(cgno);

    sysPrxUserPchar user_pchar = 0;
    sysPrxUser_pchar user_pchar_compat = user_pchar;
    sys_prx_user_pchar_t canon_pchar = user_pchar_compat;
    (void)canon_pchar;

    sysPrxUserSegmentVector seg_vec = 0;
    sys_prx_user_segment_vector_t cseg_vec = seg_vec;
    (void)cseg_vec;

    sysPrxUserPprxId p_prx_id = 0;
    sys_prx_user_p_prx_id_t cp_prx_id = p_prx_id;
    (void)cp_prx_id;

    sysPrxUserPconstVoid p_cv = 0;
    sys_prx_user_p_const_void_t cp_cv = p_cv;
    (void)cp_cv;

    sysPrxUserPstopLevel p_sl = 0;
    sys_prx_user_p_stop_level_t cp_sl = p_sl;
    (void)cp_sl;

    sysPrxModuleList pml;
    pml.size = sizeof(pml);
    sys_prx_get_module_list_t cml;
    cml.size = sizeof(cml);

    sysPrxModuleInfo pmi;
    pmi.size = sizeof(pmi);
    sys_prx_module_info_t cmi;
    cmi.size = sizeof(cmi);

    sys_prx_module_info_v2_t mi_v2;
    mi_v2.size = sizeof(mi_v2);

    /* Verify function symbol references */
    (void)sys_prx_load_module;
    (void)sys_prx_load_module_on_memcontainer;
    (void)sys_prx_load_module_by_fd;
    (void)sys_prx_load_module_on_memcontainer_by_fd;
    (void)sys_prx_load_module_list;
    (void)sys_prx_load_module_list_on_memcontainer;
    (void)sys_prx_start_module;
    (void)sys_prx_stop_module;
    (void)sys_prx_unload_module;
    (void)sys_prx_register_module;
    (void)sys_prx_get_module_list;
    (void)sys_prx_get_module_info;
    (void)sys_prx_get_module_id_by_name;
    (void)sys_prx_get_module_id_by_address;
    (void)sys_prx_get_my_module_id;
    (void)sys_prx_get_ppu_guid;
    (void)sys_prx_register_library;
    (void)sys_prx_unregister_library;
    (void)sys_prx_exitspawn_with_level;

    (void)sysPrxLoadModule;
    (void)sysPrxUnloadModule;
    (void)sysPrxStartModule;
    (void)sysPrxStopModule;
    (void)sysPrxRegisterModule;
    (void)sysPrxGetModuleList;
    (void)sysPrxGetModuleInfo;
    (void)sysPrxGetModuleIdByName;
    (void)sysPrxGetModuleId;
    (void)sysPrxGetModuleIdByAddress;

    return 0;
}

/* 6. Exercise module authoring macros */
SYS_MODULE_INFO(test_prx, 0, 1, 0);
SYS_MODULE_START(test_prx_start);
SYS_MODULE_STOP(test_prx_stop);
SYS_MODULE_EXIT(test_prx_exit);

#ifdef __cplusplus
extern "C" {
#endif
int test_prx_start(void) { return SYS_PRX_RESIDENT; }
int test_prx_stop(void) { return SYS_PRX_STOP_OK; }
int test_prx_exit(void) { return 0; }
#ifdef __cplusplus
}
#endif

int main(void)
{
    if (test_aggregate_init() != 0) return 1;
    if (test_compat_types_and_decls() != 0) return 2;
    return 0;
}
