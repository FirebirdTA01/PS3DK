# PPU binutils 2.42 patches

**Empty.** Modern binutils supports `powerpc64-ps3-elf` out of the box.

## Verification

From `bfd/config.bfd` (line 1162 in binutils 2.42):

```
powerpc64-*-elf* | powerpc-*-elf64* | powerpc64-*-linux* | \
powerpc64-*-*bsd*)
    targ_defvec=powerpc_elf64_vec
    targ_selvecs="powerpc_elf64_le_vec powerpc_elf32_vec ..."
    want64=true
    ;;
```

`powerpc64-ps3-elf` matches `powerpc64-*-elf*`. Default vector is
`powerpc_elf64_vec` (ELFv1, big-endian, 64-bit), which is exactly what the PS3
lv2 loader expects.

`gas/configure.tgt` has no PS3-specific entry but handles `powerpc64-ps3-elf`
via its `powerpc64-*-*` catchall.

`ld/configure.tgt` handles `spu-*-elf*` with the explicit upstream comment:
"This allows one to build a pair of PPU/SPU toolchains with common sysroot."
PS3 homebrew is an explicitly-supported upstream use case.

## The 2012 ps3toolchain patch

The legacy `binutils-2.22-PS3.patch` (90 lines) did NOT add
`powerpc64-ps3-elf`. It was entirely SPU-side: it added a `has_pie` /
`GENERATE_PIE_SCRIPT` mechanism to the SPU linker emulation so SPU could
produce position-independent executables.

That patch is obsolete:

1. **PS3 homebrew does not need SPU `-pie`.** SPU code uses `-fpic` (compile
   time PIC) and static linking against a fixed LS address. PIE is redundant.
2. **The `config.has_pie` mechanism no longer exists.** Binutils 2.42 gates
   `-pie` on `config.has_shared` instead. Porting the patch would require
   reimplementing it against the new mechanism for zero homebrew benefit.
3. **The Windows `execvp` const-cast fix** is also unnecessary — modern
   binutils compiles cleanly on MinGW / MSYS2.

## If you want to add SPU PIE later

Open a new patch in `patches/spu/binutils-2.42/` that:

1. Adds `GENERATE_SHLIB_SCRIPT=yes` (or a dedicated mechanism) to
   `ld/emulparams/elf32_spu.sh`, OR
2. Extends the `OPTION_PIE` case in `ld/lexsup.c` to recognize an SPU-specific
   opt-in.

Neither is required today.
