# sprx-linker (`sprxlinker`)

`sprxlinker` is the post-link fix-up step for PPU executables. It edits a
linked ELF in place so the Lv-2 loader can resolve the program's imports from
system and user PRX/SPRX modules. Every executable goes through it between
`strip` and SELF creation:

```
link -> strip -> sprxlinker -> make_self / fself
```

It is this project's fork of PSL1GHT's `sprxlinker` (MIT) and replaces the
stock copy under the same installed name, so PSL1GHT Makefile rules that
call `$(SPRX)` use it unchanged.

Source: `tools/sprx-linker/sprx-linker.c` (C, links against libelf).

## Install and locate

| Host | Location |
|---|---|
| Linux (from source) | `$PS3DEV/bin/sprxlinker` and `$PS3DK/bin/sprxlinker` |
| Windows release zip | `%PS3DK%\bin\sprxlinker.exe` |

On Linux, `scripts/install-host-tools.sh` (run by `scripts/build-sdk.sh`)
runs `make -C tools/sprx-linker install`. The Makefile builds with
`$(CC) -O2 -Wall -std=gnu99 ... -lelf` (override `CFLAGS`, `LDFLAGS`,
`LDLIBS`) and installs to `$(PS3DEV)/bin/sprxlinker` with a mirror in
`$(PS3DK)/bin` (`PS3DK` defaults to `$(PS3DEV)/ps3dk`). `make clean` removes
the local binary. The Windows `.exe` is cross-built by
`scripts/build-host-tools-windows.sh`.

## Usage

```
sprxlinker [--lp64] <elf>
```

| Argument | Meaning |
|---|---|
| `<elf>` | ELF to fix up, **modified in place** |
| `--lp64` | Accepted for old build rules; it changes nothing. Whether a call needs an LP64 TOC restore is read from the stubs themselves |

With no ELF argument it prints `Usage: sprxlinker [--lp64] [elf path]` and
exits 0. A second path is an error (`unexpected argument`).

```sh
powerpc64-ps3-elf-strip app.elf -o build/app.elf
sprxlinker build/app.elf
make_self build/app.elf app.self
fself     build/app.elf app.fake.self
```

## What it does

1. **TOC restores for tail-call import stubs.** For every `bl` in `.text`
   that targets a `.sceStub.text` trampoline of the tail-call kind (the
   import stub shape for both ILP32 and LP64), the `nop` the compiler left
   after the call becomes `ld r2,40(r1)`. Calls to the older ILP32 wrapping
   stubs, which restore r2 themselves, are left alone.
2. **Checks the process parameter section.** `.sys_proc_prx_param` must
   carry the magic `0x1b434cec` at offset 4. Both the 40-byte legacy layout
   and the 64-byte layout are accepted.
3. **Import counts.** For each record in `.lib.stub`, it counts that
   library's NIDs in `.rodata.sceFNID` and writes the count into the record.
4. **Function descriptors.** If `.opd` uses 24-byte ELFv1 descriptors, it
   packs the 32-bit function address and TOC into the third word of each,
   which is where the loader reads them. Compact 8-byte descriptors are
   left alone.

It does not rewrite `EI_OSABI`: this project's binutils already emits the
correct value.

## Outputs and errors

The only output is the edited ELF. Exit status is 0 on success and 1 on
error. Messages, all on stderr and prefixed `sprx-linker:`:

| Message | Meaning |
|---|---|
| `no .sys_proc_prx_param and no .sceStub.text — pass-through.` | Freestanding ELF with no imports; nothing to do (exit 0) |
| `elf has .sceStub.text trampolines but no .sys_proc_prx_param section ...` | The program imports functions but was not linked with the LV2 crt; its imports would never resolve (exit 1) |
| `.sys_proc_prx_param magic mismatch (expected 0x1b434cec at offset 4).` | The parameter section is not the expected one (exit 1) |
| `the import call at 0x... is followed by 0x..., not a nop, ...` / `... is the last word of .text ...` | An LP64 import call has no slot for its TOC restore (exit 1) |
| `unable to open elf file` / `libelf could not read elf file` | Bad path or not an ELF (exit 1) |

## Build integration

- **CMake:** `ps3_add_self(<target>)` in `cmake/ps3-self.cmake` runs strip,
  then `sprxlinker ${PS3_SPRXLINKER_FLAGS} <stripped elf>`, then `make_self`
  and `fself`. `PS3_SPRXLINKER_FLAGS` is a cache variable that the LP64
  toolchain file (`cmake/ps3-ppu-toolchain-lp64.cmake`) sets to `--lp64`;
  targets that add `-mlp64` to their own link options get `--lp64` too.
  Both are harmless, since the flag has no effect.
- **PSL1GHT Makefiles:** `ppu_rules` call `$(SPRX)`, which resolves to
  `$PS3DEV/bin/sprxlinker`.
- **By hand:** see the strip / sprxlinker / sign sequence above, and
  [toolchain.md](toolchain.md) for the full manual pipeline.

The ABI contract it implements is in
[docs/abi/cellos-lv2-abi-spec.md](../abi/cellos-lv2-abi-spec.md).
