/*
 * lv2-prx.S - crt object for CellOS Lv-2 relocatable modules (PRX).
 *
 * Linked into every module built with lv2-prx.ld (see ps3_add_prx in
 * cmake/ps3-self.cmake).  Emits everything a module needs that is not
 * per-module content:
 *
 *   1. The 52-byte module-info block in .rodata.sceModuleInfo
 *      (docs/local/sprx-format-facts.md §1.2).  Name, version and
 *      attributes are left zero; tools/prx-gen writes them post-link from
 *      its --name/--version/--attributes flags.  The five pointer fields
 *      are symbol references so that `-Wl,-q' retains an R_PPC64_ADDR32
 *      for each; the loader applies the relocation segment BEFORE it
 *      reads this block (facts §1.3), so link-time constants here would be
 *      read as garbage.
 *
 *   2. The 4-byte prefix words at the start of .lib.ent.top /
 *      .lib.stub.top, so exports_start/imports_start = <range start> + 4
 *      lands on the first real record (same convention as lv2-sprx.S).
 *
 *   3. Weak default module_start / module_stop that return
 *      SYS_PRX_RESIDENT (0), each with a real compact OPD descriptor, plus
 *      the nameless "management" export record (attributes 0x8000,
 *      facts §1.5/§3) carrying both under their fixed NIDs.  A module
 *      that defines either function strongly overrides the default.  The
 *      record NEVER references a symbol that could resolve to 0: the
 *      loader dereferences every address in a management record eagerly
 *      and throws on an address outside the module (facts §3, RPCS3
 *      PPUModule.cpp:795 / PPUAnalyser.h:242).
 *
 * Per-module content - the named-library export record(s) - comes from
 * <sys/prx_module.h> macros in the module's own exports.S.
 *
 * NIDs (facts §3, RPCS3 PPUFunction.cpp:13-18):
 *   module_start 0xBC9A0086   module_stop 0xAB779874
 *   (module_exit 0x3AB9A95E, module_prologue 0x0D10FD3F,
 *    module_epilogue 0x330F7005 are not emitted in v1.)
 */

	/* ------------------------------------------------------------
	 * 1. Module-info block (52 bytes).
	 * ------------------------------------------------------------ */
	.section ".rodata.sceModuleInfo","a"
	.align 2
	.globl __sys_prx_module_info
__sys_prx_module_info:
	.short 0x0000                     /* 0x00 attributes   (prx-gen) */
	.byte  0x00, 0x00                 /* 0x02 version[2]   (prx-gen) */
	.space 28, 0x00                   /* 0x04 name[28]     (prx-gen) */
	.long  .TOC.                      /* 0x20 toc                    */
	.long  __libentstart + 4          /* 0x24 exports_start          */
	.long  __libentend                /* 0x28 exports_end            */
	.long  __libstubstart + 4         /* 0x2c imports_start          */
	.long  __libstubend               /* 0x30 imports_end            */
	.size  __sys_prx_module_info, . - __sys_prx_module_info

	/* ------------------------------------------------------------
	 * 2. Range prefix words.
	 * ------------------------------------------------------------ */
	.section ".lib.ent.top","a"
	.align 2
	.long 0

	.section ".lib.stub.top","a"
	.align 2
	.long 0

	/* ------------------------------------------------------------
	 * 3a. Weak default entry points.  Shape matches what GCC 12.4
	 *     emits for `int f(void) { return 0; }' under our compact-OPD
	 *     binutils: descriptor = { .long code, .TOC. } in .opd.
	 * ------------------------------------------------------------ */
	.section ".text.module_start","ax",@progbits
	.align 2
	.weak module_start
	.section ".opd","aw"
	.align 2
module_start:
	.long .L.module_start, .TOC.
	.previous
	.type module_start, @function
.L.module_start:
	li 3, 0                           /* SYS_PRX_RESIDENT */
	blr
	.size module_start, . - .L.module_start

	.section ".text.module_stop","ax",@progbits
	.align 2
	.weak module_stop
	.section ".opd","aw"
	.align 2
module_stop:
	.long .L.module_stop, .TOC.
	.previous
	.type module_stop, @function
.L.module_stop:
	li 3, 0                           /* SYS_PRX_RESIDENT */
	blr
	.size module_stop, . - .L.module_stop

	/* ------------------------------------------------------------
	 * 3b. Management export record (facts §1.4/§1.5/§3).
	 *     44 bytes, size byte 0x2c, attributes 0x8000 with 0x0001 CLEAR.
	 *     nids[] and addrs[] are parallel, functions only.
	 * ------------------------------------------------------------ */
	.section ".rodata.sceExportNID","a"
	.align 2
.Lprx_mgmt_nids:
	.long 0xBC9A0086                  /* module_start */
	.long 0xAB779874                  /* module_stop  */
.Lprx_mgmt_nids_end:

	.section ".rodata.sceExportAddr","a"
	.align 2
.Lprx_mgmt_addrs:
	.long module_start                /* -> compact OPD entry */
	.long module_stop
.Lprx_mgmt_addrs_end:

	.section ".lib.ent","a"
	.align 2
.Lprx_mgmt_record:
	.byte  0x2c                       /* 0x00 size = 44                 */
	.byte  0x00                       /* 0x01 unk0                      */
	.short 0x0001                     /* 0x02 version                   */
	.short 0x8000                     /* 0x04 attributes: management    */
	.short (.Lprx_mgmt_nids_end - .Lprx_mgmt_nids) / 4   /* 0x06 num_func */
	.short 0x0000                     /* 0x08 num_var                   */
	.short 0x0000                     /* 0x0a num_tlsvar                */
	.byte  0x00                       /* 0x0c info_hash                 */
	.byte  0x00                       /* 0x0d info_tlshash              */
	.short 0x0000                     /* 0x0e unk1                      */
	.long  0x00000000                 /* 0x10 name: none (nameless)     */
	.long  .Lprx_mgmt_nids            /* 0x14 nids                      */
	.long  .Lprx_mgmt_addrs           /* 0x18 addrs                     */
	.long  0x00000000                 /* 0x1c vnids                     */
	.long  0x00000000                 /* 0x20 vstubs                    */
	.long  0x00000000                 /* 0x24 unk4                      */
	.long  0x00000000                 /* 0x28 unk5                      */
	.size .Lprx_mgmt_record, . - .Lprx_mgmt_record
