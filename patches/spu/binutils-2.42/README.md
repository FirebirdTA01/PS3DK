# SPU binutils 2.42 patches

Modern binutils recognizes `spu-elf` out of the box. This directory adds
SPURS extensions and an assembler correction, applied in `series` order:

- `0001`: SPURS ELF program-type flags.
- `0002`: final-content GUID generation for links defining `__SPU_GUID`.
- `0003`: rejection of conflicting SPURS modes and relocatable SPURS links,
  including options forwarded through the compiler driver.
- `0004`: keep the SPU `nop` opcode distinct from the generic `.nop`
  directive. In particular, `nop 127` is one four-byte instruction, not
  a request for 128 bytes of padding. Explicit `.nop N` retains its
  directive behavior (whole instructions, rounded up to cover N bytes).

Ordinary links retain upstream behavior. The GUID format and startup selection
contract are documented in `docs/spurs-driver-startup.md` at the repository root.

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

The SPU target remains upstream in binutils. GCC's SPU backend is
absent from modern upstream — we pin GCC 9.5.0 to work around this; a
forward-port to GCC 12+ is the long-lead stream (see
`patches/spu/FORWARD_PORT_README.md`).
