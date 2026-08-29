/*
 * sys/prx_module.h - authoring a CellOS Lv-2 relocatable module (PRX).
 *
 * Two audiences:
 *
 *   C / C++  : prototypes and return codes for module_start / module_stop.
 *   assembly : macros that emit a named-library export record for the
 *              functions a module exports (see docs/design/sprx-generation.md
 *              and docs/local/sprx-format-facts.md §1.4/§1.5).
 *
 * A module's exports live in a small exports.S next to its C sources:
 *
 *     #include <sys/prx_module.h>
 *
 *     PRX_LIBRARY_BEGIN(mylib, "mylib")
 *         PRX_EXPORT_FUNC(mylib_add,   0x1234abcd)
 *         PRX_EXPORT_FUNC(mylib_hello, 0x9876fedc)
 *     PRX_LIBRARY_END(mylib)
 *
 * The NID of each export is its FNID (nidgen fnid <name>), and the same
 * name/NID pairs go into the library's YAML so `nidgen archive' can build
 * the lib<name>_stub.a an importer links against.
 *
 * Record layout emitted per library (44 bytes, facts §1.4):
 *   size 0x2c, version 1, attributes 0x0001 (named library),
 *   num_func = number of PRX_EXPORT_FUNC entries (assembler label
 *   arithmetic - nothing is patched post-link on the export side),
 *   name -> C string, nids -> FNID array, addrs -> array of compact OPD
 *   addresses (a function symbol's value IS its descriptor address under
 *   our ELFv1 layout).
 *
 * The nameless management record (module_start / module_stop, attributes
 * 0x8000) is emitted by the crt (runtime/lv2/crt/lv2-prx.S), which also
 * provides weak defaults returning SYS_PRX_RESIDENT; define either
 * function in C to override.
 *
 * Variables / TLS exports: not supported in v1 (the schema and loader
 * support are a follow-up; see the design doc §7).
 */

#ifndef __SYS_PRX_MODULE_H__
#define __SYS_PRX_MODULE_H__

/* Return values of module_start / module_stop (facts §3.1). */
#define SYS_PRX_RESIDENT     0   /* stay loaded                           */
#define SYS_PRX_NO_RESIDENT  1   /* start failed: Lv-2 unloads the module */

/* Management-record NIDs (facts §3). */
#define SYS_PRX_NID_MODULE_START    0xBC9A0086
#define SYS_PRX_NID_MODULE_STOP     0xAB779874
#define SYS_PRX_NID_MODULE_EXIT     0x3AB9A95E
#define SYS_PRX_NID_MODULE_PROLOGUE 0x0D10FD3F
#define SYS_PRX_NID_MODULE_EPILOGUE 0x330F7005

/* Export-record attribute bits (facts §1.5). */
#define SYS_PRX_LIBATTR_LIBRARY     0x0001
#define SYS_PRX_LIBATTR_MANAGEMENT  0x8000

#ifdef __ASSEMBLER__

/*
 * PRX_LIBRARY_BEGIN(tag, "name")
 *   tag  - an identifier unique within this .S file (used for labels)
 *   name - the library name string an importer's .lib.stub refers to
 */
#define PRX_LIBRARY_BEGIN(tag, name)                                    \
	.section ".rodata.sceExportName","a";                           \
	.align 2;                                                       \
	.Lprx_name_##tag: .asciz name;                                  \
	.section ".rodata.sceExportNID","a";                            \
	.align 2;                                                       \
	.Lprx_nids_##tag:;                                              \
	.section ".rodata.sceExportAddr","a";                           \
	.align 2;                                                       \
	.Lprx_addrs_##tag:

/*
 * PRX_EXPORT_FUNC(symbol, nid)
 *   Appends one function export.  `symbol' must be a global function
 *   defined in this module; its value is the compact OPD descriptor the
 *   importer's trampoline dereferences.
 */
#define PRX_EXPORT_FUNC(sym, nid)                                       \
	.section ".rodata.sceExportNID","a";                            \
	.long nid;                                                      \
	.section ".rodata.sceExportAddr","a";                           \
	.long sym

/*
 * PRX_LIBRARY_END(tag)
 *   Closes the arrays and emits the 44-byte .lib.ent record.
 */
#define PRX_LIBRARY_END(tag)                                            \
	.section ".rodata.sceExportNID","a";                            \
	.Lprx_nids_end_##tag:;                                          \
	.section ".rodata.sceExportAddr","a";                           \
	.Lprx_addrs_end_##tag:;                                         \
	.section ".lib.ent","a";                                        \
	.align 2;                                                       \
	.Lprx_record_##tag:;                                            \
	.byte  0x2c;                            /* size         */      \
	.byte  0x00;                            /* unk0         */      \
	.short 0x0001;                          /* version      */      \
	.short SYS_PRX_LIBATTR_LIBRARY;         /* attributes   */      \
	.short (.Lprx_nids_end_##tag - .Lprx_nids_##tag) / 4; /* num_func */ \
	.short 0x0000;                          /* num_var      */      \
	.short 0x0000;                          /* num_tlsvar   */      \
	.byte  0x00;                            /* info_hash    */      \
	.byte  0x00;                            /* info_tlshash */      \
	.short 0x0000;                          /* unk1         */      \
	.long  .Lprx_name_##tag;                /* name         */      \
	.long  .Lprx_nids_##tag;                /* nids         */      \
	.long  .Lprx_addrs_##tag;               /* addrs        */      \
	.long  0x00000000;                      /* vnids        */      \
	.long  0x00000000;                      /* vstubs       */      \
	.long  0x00000000;                      /* unk4         */      \
	.long  0x00000000                       /* unk5         */

#else /* C / C++ */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Module entry points.  Called by the guest-side sys_prx_start_module /
 * sys_prx_stop_module wrappers (facts §3.1).  Return SYS_PRX_RESIDENT on
 * success; any other value from module_start makes Lv-2 unload the module
 * immediately and is reported to the caller as the start error.
 */
int module_start(size_t args, void *argp);
int module_stop(size_t args, void *argp);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */

#endif /* __SYS_PRX_MODULE_H__ */
