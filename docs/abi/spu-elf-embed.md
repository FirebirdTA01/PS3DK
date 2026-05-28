# SPU ELF embed format

This document specifies the `--format=elf` artifact emitted by
`spu-elf-to-ppu-obj`.  The format embeds a hard-stripped linked SPU ELF in a
PPU relocatable object.

This document covers `--format=elf` (hard-stripped ELF embed).  For
`--format=binary` (flat LS image embed), see
[spu-elf-binary-embed.md](spu-elf-binary-embed.md).  For `--format=jobbin2`,
see [spurs-jobbin2-wrapper-abi.md](spurs-jobbin2-wrapper-abi.md).

The output object contains:

- `.spu_image`: the hard-stripped SPU ELF, padded to the section alignment.
- `_binary_<name>_elf_start`, `_binary_<name>_elf_end`, and
  `_binary_<name>_elf_size` symbols using objcopy-style names.

The ELF embed format does not emit a jobbin2 wrapper prefix, a
`.spu_image.jobheader` section, or a jobheader relocation.

## SPU Input Requirements

The linked SPU ELF must satisfy:

- ELF32, big-endian, `EM_SPU`, `ET_EXEC`.
- `e_flags = 0`.
- The program header table is within the input file.
- Each program-header file range `p_offset..p_offset+p_filesz` is within the
  input file.

`e_flags = 1` and `e_flags = 2` SPURS images are not ELF-embed inputs.  Those
forms use the JOBBIN and jobbin2 paths respectively.

## Hard-Strip Transform

The embedded payload is a hard-stripped SPU ELF image.  The transform is:

- Copy bytes `0..max(p_offset + p_filesz)` across all program headers.
- Preserve as-is: `e_ident`, `e_type`, `e_machine`, `e_version`, `e_entry`,
  `e_phoff`, `e_flags`, `e_ehsize`, `e_phentsize`, `e_phnum`, and the program
  header table.  `e_shentsize` is also preserved even though the stripped
  output has no section headers.
- Zero in place: `e_shoff`, `e_shnum`, and `e_shstrndx`.
- Drop entirely: section header table bytes and everything referenced only by
  section headers, including `.symtab`, `.strtab`, `.shstrtab`, `.comment`,
  and debug sections.

The resulting SPU ELF has no section headers.  Its program headers remain the
authoritative runtime view.

## PPU Object Contract

The PPU relocatable object is ELF64, big-endian, `EM_PPC64`.

`.spu_image` contains the hard-stripped SPU ELF and is aligned to `0x80`.
The section may include zero padding after the hard-stripped ELF to satisfy
section alignment.  The `_binary_<name>_elf_end` and
`_binary_<name>_elf_size` symbols refer to the meaningful hard-stripped ELF
byte count, not the padded section size.

| Symbol | Section | Value | Size |
|---|---|---:|---:|
| `_binary_<name>_elf_start` | `.spu_image` | `0` | hard-stripped ELF size |
| `_binary_<name>_elf_end` | `.spu_image` | hard-stripped ELF size | `0` |
| `_binary_<name>_elf_size` | absolute | hard-stripped ELF size | `0` |

The symbol base is the caller-provided `--symbol-base` after replacing any
non-ASCII-alphanumeric, non-underscore byte with `_`.

This encoder uses the caller-provided `--symbol-base` for the
`_binary_<name>_elf_*` symbols and does not emit a `STT_FILE` symbol.  The
reference helper's `STT_FILE` symbol is build metadata only and is not part of
the runtime contract.

## Version Sections

The independent encoder does not emit `.scespuversion` or
`.debug_scespuversion` for ELF embed format.  Those sections are diagnostic and
are not required by the current consumers.  If a future consumer requires
them, add an explicit positive option such as `--emit-version-sections`.

## Coverage

This specification covers default `--format=elf` embeds for `e_flags = 0`
SPU ELFs.  Reference Makefile-based samples use this default form with
objcopy-style symbols.

This specification does not cover custom symbol names, writable sections,
section-alignment overrides, explicit strip-mode selection, or version-section
emission.  Those options are not used by the current Makefile-based reference
sample set and should be specified only when a concrete consumer requires
them.
