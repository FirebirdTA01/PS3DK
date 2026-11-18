# spu-elf-to-ppu-obj

PPU code that starts SPU threads, SPURS tasks or SPURS jobs needs the SPU
program's image inside the PPU executable. `spu-elf-to-ppu-obj` takes a linked
SPU ELF and writes a PPU relocatable object that holds the image and exposes it
through `_binary_<name>_*` symbols. Unlike a plain `bin2s` embedding, it turns
each PPU-symbol reference in the SPU image (`CELL_SPURS_PPU_SYM`) into a PPU
relocation, so the PPU link fills the addresses in. It also inspects existing
artifacts and checks images that must not carry PPU references.

Source: `tools/spu-elf-to-ppu-obj` (Rust workspace).

## Install and locate

`scripts/build-sdk.sh` installs it as `$PS3DEV/bin/spu-elf-to-ppu-obj` and
`$PS3DK/bin/spu-elf-to-ppu-obj`; the Windows SDK zip ships
`%PS3DK%\bin\spu-elf-to-ppu-obj.exe`. Build it alone with
`cargo build --release --manifest-path tools/Cargo.toml -p spu-elf-to-ppu-obj`.
`--version` and `--help` work on the tool and on each subcommand.

## wrap

```
spu-elf-to-ppu-obj wrap --spu-elf <in.elf> --output <out.o> --symbol-base <name>
                        [--format jobbin2|binary|elf|task] [--emit-sidecars]
```

| Option | Meaning |
|---|---|
| `--spu-elf <path>` | Required. Linked SPU ELF (ELF32 big-endian `EM_SPU` executable). |
| `--output <path>` | Required. PPU relocatable object to write; missing parent directories are created. |
| `--symbol-base <name>` | Required. The `<name>` in the `_binary_<name>_*` symbols. |
| `--format <f>` | Embed format, default `jobbin2` (below). |
| `--emit-sidecars` | Also write the raw image next to the output (below). |

On success it prints `wrote <paths>` to stderr.

### Formats

Every format puts the image in a `.spu_image` section aligned to 0x80 bytes.

| Format | Input | Image | Symbols |
|---|---|---|---|
| `jobbin2` (default) | A SPURS job ELF with `e_flags` 1 (job binary) or 2 (job-queue job with its startup code) | The jobbin2 blob: a 0x100-byte ELF32/SPU prefix followed by the final local-store image; a `CellSpursJobHeader` template in `.spu_image.jobheader` | `_binary_<name>_jobbin2_start`, `_end`, `_size`, `_jobheader` |
| `binary` | Any SPU executable | The flat local-store image; an `e_flags` 2 job also gets its job-info trailer and must be linked from LS 0 | `_binary_<name>_bin_start`, `_end`, `_size` |
| `elf` | Any SPU executable | The SPU ELF, hard-stripped | `_binary_<name>_elf_start`, `_end`, `_size` |
| `task` | A SPURS task ELF (`e_flags` 3) | The hard-stripped ELF; a `CellSpursTaskBinInfo` in `.spu_image.taskbininfo` | `_binary_<name>_start`, `_end`, `_size`, `_taskbininfo` |

`jobbin2` cannot carry PPU symbol references yet; such an image is refused
with a message suggesting `--format=binary` (or taking the address from the
job descriptor). The other formats carry them as PPU relocations.

### Sidecars

`--emit-sidecars` writes, next to the output with its extension removed:

| Format | Sidecars |
|---|---|
| `jobbin2` | `<base>.jobbin2` (the blob) and `<base>_jobheader.bin` |
| `binary` | `<base>.bin` |
| `elf`, `task` | `<base>.elf` (the stripped ELF) |

## inspect

```
spu-elf-to-ppu-obj inspect --spu-elf <in.elf> [--jobbin2 <blob>] [--ppu-obj <obj>] [--jobheader <bin>]
```

Prints a JSON report to stdout: the SPU ELF summary (entry, flags, program
headers, symbols), a summary of each optional artifact, the job-queue metadata
patches for an `e_flags` 2 image, and a list of checks marked `pass`, `fail`
or `skip`. The checks cover, among others: entry and flags agree between the
ELF and the blob, the payload bytes match the expected local-store image,
`.spu_image` covers the image and is 0x80-aligned, the `_size` symbols match,
and the job-header relocation is
`R_PPC64_ADDR32 _binary_<name>_jobbin2_start + 0x100`. `--spu-elf` is
required; each optional input adds its checks, and a check whose input is
missing is reported as `skip`.

```cmd
spu-elf-to-ppu-obj.exe wrap --spu-elf worker.spu.elf --output worker_img.o --symbol-base worker --emit-sidecars
spu-elf-to-ppu-obj.exe inspect --spu-elf worker.spu.elf --ppu-obj worker_img.o --jobbin2 worker_img.jobbin2 --jobheader worker_img_jobheader.bin
```

## no-ppu-refs

```
spu-elf-to-ppu-obj no-ppu-refs --spu-elf <in.elf> [--stamp <file>]
```

Fails when the SPU ELF refers to PPU symbols, listing each reference
(`LS <addr>: <bits>-bit address of PPU symbol <name>`), because an embedding
that copies raw bytes cannot pass them to the PPU link. On success it creates
the empty `--stamp` file, if one is given.

## Errors

Every failure prints `spu-elf-to-ppu-obj: <reason>` and exits 2. The input
checks refuse: a file that is not an ELF32 big-endian `EM_SPU` executable;
`PT_LOAD` segments with `p_vaddr != p_paddr`; `e_entry` not equal to `_start`;
an unsupported job `e_flags` (jobbin2 takes 1 and 2); a job-binary image
without its `bin2` marker (or zeros) at image offset 0x20; a job-queue image
without its startup code at LS 0x30; a missing or unaligned
`__bss_start`/`_end` extent; and a task ELF with no `PT_LOAD` segment.

## Build integration

You rarely call it directly. `ps3_add_spu_image` in `cmake/ps3-self.cmake`
compiles SPU sources with `spu-elf-gcc`, links them and embeds the result in a
PPU target:

```cmake
ps3_add_spu_image(<target> NAME <name> SOURCES <spu sources...>
    [LIBS <libs...>] [CFLAGS <flags...>] [LDFLAGS <flags...>] [LDSCRIPT <file>]
    [NOSTARTFILES] [FREESTANDING]
    [JOBBIN | JOBBIN_WRAP | PPU_OBJECT]
    [OVIS_CONFIG <file.xml> | OVIS_AUTO <sources...>])
```

| Embedding | How the image is embedded | PPU names |
|---|---|---|
| default | The SPU ELF through `bin2s` | `<name>_bin`, `<name>_bin_end`, `<name>_bin_size` |
| `JOBBIN` | A flat image from `spu-elf-objcopy -O binary` (job-chain jobs, linked where the job manager loads them, LS 0x4c00), through `bin2s` | the `bin2s` names of `<name>.bin` |
| `JOBBIN_WRAP` | `wrap --emit-sidecars` (jobbin2); the blob (copied to `<name>.bin`) and `<name>_jobheader.bin` go through `bin2s` | the `bin2s` names of both files |
| `PPU_OBJECT` | `wrap --format elf`; the object is linked into the target and the generated `<name>_bin.h` maps the `bin2s` names to the `_binary_<name>_elf_*` symbols as macros | `<name>_bin`, `<name>_bin_end`, `<name>_bin_size` |

Use `PPU_OBJECT` when the SPU code holds PPU addresses
(`CELL_SPURS_PPU_SYM`); it does not combine with `JOBBIN` or `JOBBIN_WRAP`.
When the tool is installed, the raw-byte paths (default and `JOBBIN`) run
`no-ppu-refs` on the linked image and refuse one with PPU references instead
of shipping it with the addresses left 0. `JOBBIN_WRAP` and `PPU_OBJECT` fail
at configure time without the tool.

The other options: `LIBS` are `-l` names searched in the SPU library
directories (two or more are wrapped in `--start-group`/`--end-group`);
`CFLAGS` and `LDFLAGS` are appended; `LDSCRIPT` adds `-T <file>`;
`NOSTARTFILES` adds `-nostartfiles`; `FREESTANDING` compiles with
`-ffreestanding -fno-exceptions` instead of the default `-fpic
-fno-exceptions` (plus `-fno-rtti` for C++). Jobs (`JOBBIN`, `JOBBIN_WRAP`)
link with `-Wl,--require-defined=_start`. The overlay options are described in
[ovis.md](ovis.md).

Worked examples are under `samples/spu/` and `samples/spurs/`;
`samples/spurs/hello-spurs-jq` and `samples/spurs/spurs-status` use the
wrapper paths.
