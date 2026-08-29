/*
 * lv2-prx.ld - linker script for CellOS Lv-2 relocatable modules (PRX).
 *
 * Derived from lv2.ld (the executable script).  A PRX is linked as a
 * normal ET_EXEC image at base 0 with `-Wl,-q' (--emit-relocs) and then
 * post-processed by tools/prx-gen into the module format the Lv-2 loader
 * accepts (docs/design/sprx-generation.md, docs/local/sprx-format-facts.md).
 *
 * Deliberate differences from lv2.ld:
 *
 *   - No ENTRY.  A module has no process entry point; the loader reaches
 *     module_start through the management export record emitted by
 *     lv2-prx.S (NID 0xBC9A0086).  prx-gen writes e_entry = 0.
 *
 *   - Base address 0 and exactly ONE PT_LOAD segment (flags RWX).  The
 *     loader relocates every PRX segment independently; with a single
 *     segment every relocation entry in the 0x700000A4 segment has
 *     index_addr == index_value == 0 and its addend is the link-time
 *     address itself, which removes a whole class of conversion bugs.
 *     Splitting into RX/RW segments is a follow-up once the reference
 *     module layout has been read on CachyOS.
 *
 *   - No hdr_param / hdr_prx / hdr_tls program headers.  The loader's
 *     segment switch accepts only PT_LOAD and 0x700000A4; anything else is
 *     "Unknown segment type".  A PRX carries no .sys_proc_prx_param; the
 *     52-byte module-info block (.rodata.sceModuleInfo) takes that role and
 *     is found through PHDR[0].p_paddr, which prx-gen fills in.
 *
 *   - No .rel.dyn / .rela.dyn merge blocks.  Under -q the linker keeps one
 *     .rela.<section> per patched output section; merging them into
 *     .rela.dyn destroys the per-target sh_info prx-gen relies on to know
 *     which section a relocation patches.
 *
 *   - .rodata.sceModuleInfo is the first section after the ELF/program
 *     headers so that it trivially lies inside PHDR[0].
 *
 *   - Export-table sections (.rodata.sceExportName / .sceExportNID /
 *     .sceExportAddr) are placed and KEEP'd; they are emitted by
 *     <sys/prx_module.h> macros and by the management record in lv2-prx.S.
 *
 * Everything the executable script does for the import machinery
 * (.sceStub.text, .rodata.sceResident/.sceFNID, .lib.stub.*, .data.sceFStub,
 * compact .opd at ALIGN(4), .got/.toc and __lv2_toc_bias) is preserved
 * verbatim so that lib*_stub.a archives link into a module unchanged.
 *
 * TLS: .tdata/.tbss are placed (not discarded) so that prx-gen can refuse a
 * module that uses TLS with a clear message instead of silently dropping it.
 */

OUTPUT_FORMAT("elf64-powerpc", "elf64-powerpc", "elf64-powerpc")
OUTPUT_ARCH(powerpc:common64)

PHDRS
{
	hdr_load	PT_LOAD FILEHDR PHDRS	FLAGS(0x01 | 0x02 | 0x04);
}

SECTIONS
{
	. = 0 + SIZEOF_HEADERS;

	/* --- module-info block: first thing inside PHDR[0] ----------------- */
	.rodata.sceModuleInfo : { KEEP (*(.rodata.sceModuleInfo)) } : hdr_load

	/* --- code ----------------------------------------------------------- */
	.init           : {
		KEEP (*(.init))
	} = 0x60000000
	.text           : {
		*(.text .stub .text.* .gnu.linkonce.t.*)
		KEEP (*(.text.*personality*))
		*(.gnu.warning)
		*(.sfpr .glink)
	} = 0x60000000
	.fini           : {
		KEEP (*(.fini))
	} = 0x60000000
	.sceStub.text   : {
		*(.sceStub.text)
		*(SORT(.sceStub.text.*))
	}
	PROVIDE (__etext = .);
	PROVIDE (_etext = .);
	PROVIDE (etext = .);

	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame       : ONLY_IF_RO { KEEP (*(.eh_frame)) }
	.gcc_except_table   : ONLY_IF_RO { *(.gcc_except_table .gcc_except_table.*) }

	/* --- import metadata (nidgen stub archives, unchanged) --------------- */
	.rodata.sceResident : { KEEP (*(.rodata.sceResident)) }
	.rodata.sceFNID     : { KEEP (*(.rodata.sceFNID .rodata.sceFNID.*)) }
	.rodata.sceVNID     : { KEEP (*(.rodata.sceVNID .rodata.sceVNID.*)) }
	.rodata.sceTNID     : { KEEP (*(.rodata.sceTNID .rodata.sceTNID.*)) }
	.rodata.sceVStub    : { KEEP (*(.rodata.sceVStub)) KEEP (*(SORT(.rodata.sceVStub.*))) }

	/* --- export metadata (<sys/prx_module.h> + lv2-prx.S) ---------------- */
	.rodata.sceExportName : { KEEP (*(.rodata.sceExportName .rodata.sceExportName.*)) }
	. = ALIGN(4);
	.rodata.sceExportNID  : { KEEP (*(.rodata.sceExportNID .rodata.sceExportNID.*)) }
	.rodata.sceExportAddr : { KEEP (*(.rodata.sceExportAddr .rodata.sceExportAddr.*)) }

	. = ALIGN(4);
	PROVIDE (__libentstart = .);
	.lib.ent.top : { KEEP (*(.lib.ent.top)) }
	.lib.ent	 : { KEEP (*(.lib.ent)) }
	.lib.ent.btm : { KEEP (*(.lib.ent.btm)) }
	PROVIDE (__libentend = .);

	. = ALIGN(4);
	PROVIDE (__libstubstart = .);
	.lib.stub.top : { KEEP (*(.lib.stub.top)) }
	.lib.stub	  : { KEEP (*(.lib.stub)) }
	.lib.stub.btm : { KEEP (*(.lib.stub.btm)) }
	PROVIDE (__libstubend = .);

	.ppu_rodata         : { *(.ppu_rodata .ppu_rodata.*) }

	.rodata         : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
	.rodata1        : { *(.rodata1) }
	.sdata2         : { *(.sdata2 .sdata2.* .gnu.linkonce.s2.*) }
	.sbss2          : { *(.sbss2 .sbss2.* .gnu.linkonce.sb2.*) }

	/* --- data ------------------------------------------------------------ */
	. = ALIGN(16);
	.ppu_data           : { *(.ppu_data .ppu_data.*) }

	.eh_frame       : ONLY_IF_RW { KEEP (*(.eh_frame)) }
	.gcc_except_table   : ONLY_IF_RW { *(.gcc_except_table .gcc_except_table.*) }

	. = ALIGN(8);
	.preinit_array     : {
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP (*(.preinit_array))
		PROVIDE_HIDDEN (__preinit_array_end = .);
	}
	.init_array     : {
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array))
		PROVIDE_HIDDEN (__init_array_end = .);
	}
	.fini_array     : {
		PROVIDE_HIDDEN (__fini_array_start = .);
		KEEP (*(.fini_array))
		KEEP (*(SORT(.fini_array.*)))
		PROVIDE_HIDDEN (__fini_array_end = .);
	}
	.ctors          : {
		KEEP (*crtbegin.o(.ctors))
		KEEP (*crtbegin?.o(.ctors))
		KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
		KEEP (*(SORT(.ctors.*)))
		KEEP (*(.ctors))
	}
	.dtors          : {
		KEEP (*crtbegin.o(.dtors))
		KEEP (*crtbegin?.o(.dtors))
		KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
		KEEP (*(SORT(.dtors.*)))
		KEEP (*(.dtors))
	}
	.jcr            : { KEEP (*(.jcr)) }
	.data.rel.ro : { *(.data.rel.ro.local*) *(.data.rel.ro*) }
	.dynamic        : { *(.dynamic) }

	.data.sceFStub      : { KEEP (*(.data.sceFStub)) KEEP (*(SORT(.data.sceFStub.*))) }

	.toc1		  : ALIGN(8) { *(.toc1) }
	.opd		  : ALIGN(4) { KEEP (*(.opd)) }
	.branch_lt	  : ALIGN(8) { *(.branch_lt) }
	__lv2_got_start = .;
	.got		  : ALIGN(8) { *(.got .toc) }
	__lv2_toc_bias = __lv2_got_start + 0x8000 - .TOC.;
	.toc.sceStub  : { KEEP (*(.toc.sceStub)) KEEP (*(SORT(.toc.sceStub.*))) }

	/* TLS: placed so prx-gen can see and refuse it (v1 has no TLS support). */
	.tdata	  : { *(.tdata .tdata.* .gnu.linkonce.td.*) }
	.tbss		  : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) }

	.data           :
	{
		*(.data .data.* .gnu.linkonce.d.*)
		KEEP (*(.gnu.linkonce.d.*personality*))
		SORT(CONSTRUCTORS)
	}
	.data1          : { *(.data1) }
	.sdata          : {
		*(.sdata .sdata.* .gnu.linkonce.s.*)
	}
	_edata = .;
	PROVIDE (edata = .);

	__bss_start = .;
	.tocbss	  : ALIGN(8) { *(.tocbss)}
	.sbss           :
	{
		PROVIDE (__sbss_start = .);
		PROVIDE (___sbss_start = .);
		*(.dynsbss)
		*(.sbss .sbss.* .gnu.linkonce.sb.*)
		*(.scommon)
		PROVIDE (__sbss_end = .);
		PROVIDE (___sbss_end = .);
	}
	.bss            :
	{
		*(.dynbss)
		*(.bss .bss.* .gnu.linkonce.b.*)
		*(COMMON)
		. = ALIGN(64 / 8);
	}
	_end = .;
	__end = .;
	PROVIDE (end = .);

	/* Stabs debugging sections.  */
	.stab          0 : { *(.stab) }
	.stabstr       0 : { *(.stabstr) }
	.stab.excl     0 : { *(.stab.excl) }
	.stab.exclstr  0 : { *(.stab.exclstr) }
	.stab.index    0 : { *(.stab.index) }
	.stab.indexstr 0 : { *(.stab.indexstr) }
	.comment       0 : { *(.comment) }
	/* DWARF debug sections (addresses relative to section start). */
	.debug          0 : { *(.debug) }
	.line           0 : { *(.line) }
	.debug_srcinfo  0 : { *(.debug_srcinfo) }
	.debug_sfnames  0 : { *(.debug_sfnames) }
	.debug_aranges  0 : { *(.debug_aranges) }
	.debug_pubnames 0 : { *(.debug_pubnames) }
	.debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
	.debug_abbrev   0 : { *(.debug_abbrev) }
	.debug_line     0 : { *(.debug_line) }
	.debug_frame    0 : { *(.debug_frame) }
	.debug_str      0 : { *(.debug_str) }
	.debug_loc      0 : { *(.debug_loc) }
	.debug_macinfo  0 : { *(.debug_macinfo) }
	.debug_weaknames 0 : { *(.debug_weaknames) }
	.debug_funcnames 0 : { *(.debug_funcnames) }
	.debug_typenames 0 : { *(.debug_typenames) }
	.debug_varnames  0 : { *(.debug_varnames) }
	.debug_pubtypes 0 : { *(.debug_pubtypes) }
	.debug_ranges   0 : { *(.debug_ranges) }
	.gnu.attributes 0 : { KEEP (*(.gnu.attributes)) }
	/DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) }
}
