/*
 * lv2-sprx.S — native CellOS Lv-2 process parameter block + library
 *              table header prefixes.
 *
 * Emits the .sys_proc_prx_param section per
 * docs/abi/cellos-lv2-abi-spec.md (64 bytes, magic 0x1b434cec) and
 * the mandatory 4-byte headers at the start of .lib.ent.top and
 * .lib.stub.top so the proc_param's +4 addends resolve correctly.
 *
 * Weak defaults are provided for __PPU_GUID and __sys_process_enabled
 * so a minimal sample links without the user having to define them.
 * A non-trivial project will override either symbol at link time.
 *
 * Build-time knobs:
 *   LV2_SDK_VERSION  BCD-encoded SDK version word (default 0x00475001).
 */

#ifndef LV2_SDK_VERSION
#define LV2_SDK_VERSION 0x00475001
#endif

	/* ------------------------------------------------------------
	 * .lib.ent.top / .lib.stub.top header prefixes.
	 *
	 * The linker script gathers these sections before .lib.ent / .lib.stub
	 * proper. The 4-byte filler lets .sys_proc_prx_param reference
	 * __libentstart+4 / __libstubstart+4 — which is what the Lv-2 loader
	 * expects to find the first real library entry / stub descriptor.
	 * ------------------------------------------------------------ */
	.section ".lib.ent.top","a"
	.align 2
	.long 0

	.section ".lib.stub.top","a"
	.align 2
	.long 0

	/* ------------------------------------------------------------
	 * Weak default symbols.
	 *
	 * __PPU_GUID carries the process's unique identifier. Shipping
	 * projects override with their own 32-bit GUID.
	 *
	 * __sys_process_enabled is a process-capability word; the loader
	 * reads it to decide which Lv-2 services the process may use.
	 * Default 0 means "baseline set only".
	 * ------------------------------------------------------------ */
	.section ".data","aw"
	.align 2
	.weak __PPU_GUID
	.type __PPU_GUID, @object
	.size __PPU_GUID, 4
__PPU_GUID:
	.long 0

	.align 2
	.weak __sys_process_enabled
	.type __sys_process_enabled, @object
	.size __sys_process_enabled, 4
__sys_process_enabled:
	.long 0

	/* ------------------------------------------------------------
	 * .sys_proc_prx_param — 64 bytes, spec section 3.
	 * Every pointer field is a 32-bit R_PPC64_ADDR32 reloc.
	 * ------------------------------------------------------------ */
	.section ".sys_proc_prx_param","a"
	.align 2
	.long 0x00000040                  /* 0x00 size = 64 */
	.long 0x1b434cec                  /* 0x04 magic */
	.long 0x00000004                  /* 0x08 version */
	.long LV2_SDK_VERSION             /* 0x0c sdk_version */
	.long __libentstart + 4           /* 0x10 lib_ent.begin */
	.long __libentend                 /* 0x14 lib_ent.end */
	.long __libstubstart + 4          /* 0x18 lib_stub.begin */
	.long __libstubend                /* 0x1c lib_stub.end */
	.long 0x00000000                  /* 0x20 reserved / flags */
	.long __PPU_GUID                  /* 0x24 ppu_guid_addr */
	.long __sys_process_enabled       /* 0x28 sys_process_enable */
	.long 0x00000000                  /* 0x2c reserved */
	.long 0x00000000                  /* 0x30 reserved */
	.long 0x00000000                  /* 0x34 reserved */
	.long 0x00000000                  /* 0x38 reserved */
	.long 0x00000000                  /* 0x3c reserved */
