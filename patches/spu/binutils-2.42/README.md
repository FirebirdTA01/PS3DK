# SPU binutils 2.42 patches

**Empty.** Modern binutils supports `spu-elf` out of the box.

## Verification

From `ld/configure.tgt`:

```
spu-*-elf*)    targ_emul=elf32_spu
```

```
spu-*-elf*)
  # This allows one to build a pair of PPU/SPU toolchains with common sysroot.
  NATIVE_LIB_DIRS='/lib'
  ;;
```

`ld/emulparams/elf32_spu.sh`, `ld/emultempl/spuelf.em`, and the SPU overlay
support (`spu_icache.S`, `spu_ovl.S`) are all present.

`gas`, `bfd`, `opcodes` all handle SPU opcodes and ELF variants.

## What's missing upstream

The assembler/linker story for SPU is complete. Only GCC's SPU backend is
absent from modern upstream — we pin GCC 9.5.0 to work around this; a
forward-port to GCC 12+ is the long-lead stream (see
`patches/spu/FORWARD_PORT_README.md`).
