# SPU ELF binary embed format

This document specifies the `--format=binary` artifact emitted by
`spu-elf-to-ppu-obj`.  The format embeds a linked SPU ELF's materialized
local-store image in a PPU relocatable object.

This document covers `--format=binary` (flat LS image embed).  For
`--format=elf` (hard-stripped ELF embed), see
[spu-elf-embed.md](spu-elf-embed.md).  For `--format=jobbin2`, see
[spurs-jobbin2-wrapper-abi.md](spurs-jobbin2-wrapper-abi.md).

The output object contains:

- `.spu_image`: the SPU local-store image, padded to the section alignment.
- `_binary_<name>_bin_start`, `_binary_<name>_bin_end`, and
  `_binary_<name>_bin_size` symbols using objcopy-style names.

The binary format does not emit a jobbin2 wrapper prefix, a
`.spu_image.jobheader` section, or a jobheader relocation.

## SPU Input Requirements

The linked SPU ELF must satisfy:

- ELF32, big-endian, `EM_SPU`, `ET_EXEC`.
- `e_entry` is nonzero and equals `_start`.
- `PT_LOAD` segments use `p_vaddr == p_paddr` and describe the local-store
  addresses to materialize.

`e_flags` does not select this format.  Callers request it explicitly with
`spu-elf-to-ppu-obj wrap --format=binary`.  The current encoder accepts
`e_flags = 0` and `e_flags = 1` inputs.  It rejects `e_flags = 2` inputs,
because that SPURS with-CRT job form requires additional SPURS JOB INFO
metadata that is not specified here.

## Local-Store Image

The encoder creates a zero-filled byte array with length:

```text
ls_size = align16(max(p_paddr + p_memsz) across PT_LOAD segments)
```

For each `PT_LOAD`, it copies `p_filesz` bytes from the SPU ELF to
`image[p_paddr..p_paddr+p_filesz]` and leaves `p_memsz - p_filesz` as zero
fill.  The embedded image therefore includes the zeroed BSS extent.  This is
different from a raw `spu-elf-objcopy -O binary` file, which may stop at the
last file-backed byte and omit trailing zero fill.

No jobbin2 runtime metadata patches are applied in binary format.

## PPU Object Contract

The PPU relocatable object is ELF64, big-endian, `EM_PPC64`.

`.spu_image` contains the materialized LS image and is aligned to `0x80`.
The section may include zero padding after `ls_size` to satisfy section
alignment.  The `_binary_<name>_bin_end` and `_binary_<name>_bin_size` symbols
refer to the meaningful LS image byte count, not the padded section size.

| Symbol | Section | Value | Size |
|---|---|---:|---:|
| `_binary_<name>_bin_start` | `.spu_image` | `0` | `ls_size` |
| `_binary_<name>_bin_end` | `.spu_image` | `ls_size` | `0` |
| `_binary_<name>_bin_size` | absolute | `ls_size` | `0` |

The symbol base is the caller-provided `--symbol-base` after replacing any
non-ASCII-alphanumeric, non-underscore byte with `_`.

## Version Sections

The independent encoder does not emit `.scespuversion` or
`.debug_scespuversion` for binary format.  Those sections are diagnostic and
are not required by the current consumers.  If a future consumer requires
them, add an explicit positive option such as `--emit-version-sections`.

## Coverage

This specification covers flat binary embeds for `e_flags = 0` and
`e_flags = 1` SPU ELFs.  These are the forms used by the current
`--format=binary` consumer samples.

`e_flags = 2` SPURS with-CRT jobs are deferred.  Black-box comparison shows
that the binary embed for that form carries an inline SPURS JOB INFO metadata
block in the `.spu_image` payload.  The block starts with ASCII bytes
`25 53 50 55 52 53 20 4a 4f 42 20 49 4e 46 4f 25`
(`%SPURS JOB INFO%`) and is not removed by the reference helper's
`--disable-extra-data` flag.  Observed placement is still under investigation:
one baseline image placed the block at `output_size - 0x60`; images with
forced BSS growth placed it at `output_size - 0x70`.  Encoders must reject
`e_flags = 2` binary embeds until this metadata block is specified.
