# SPURS jobbin2 wrapper and jobheader ABI

This document specifies the jobbin2 artifact emitted for SPURS job-queue SPU
workloads.  The artifact is stored in a PPU relocatable object as two sections:

- `.spu_image`: a jobbin2 wrapper blob.  The first `0x100` bytes are an
  ELF32/SPU wrapper prefix.  The SPU local-store image starts at blob offset
  `0x100`.
- `.spu_image.jobheader`: a `0x30`-byte `CellSpursJobHeader` template.  The
  template initializes the fields the PPU queues with each job descriptor;
  descriptor alignment padding is not serialized.

A jobbin2 encoder derives the SPU local-store image from the linked SPU ELF's
`PT_LOAD` segments, applies the JQ runtime metadata patches specified below,
and embeds the patched image at `blob + 0x100`.

This document covers the job-queue with-CRT form (`e_flags = 2`), whose runtime
stub carries the `JOBCRT Ver13` marker.  The flat `JOBBIN` job-chain form
(`e_flags = 1`) uses a different image path and may carry the `bin2` convention
specified in [spurs-job-entry-point.md](spurs-job-entry-point.md).  One
important asymmetry: the `e_flags = 1` runtime stub emits the `bin2` validator
slot at link time, while the `e_flags = 2` jobbin2 path supplies that slot
during wrapper encoding.

## SPU Input Requirements

The linked SPU ELF must satisfy:

- ELF32, big-endian, `EM_SPU`, `ET_EXEC`.
- `e_entry` is nonzero and equals `_start`.
- `e_flags` is a supported SPURS job value.  For job-queue workloads this is
  `2`, the with-CRT job form.
- `PT_LOAD` segments use `p_vaddr == p_paddr` and describe the local-store
  addresses to materialize.
- The materialized image contains `.SpuGUID` at LS `0`.
- The materialized image contains the `JOBCRT Ver13` marker defined by
  `libspurs_jq`'s `job_start_w_crt` entry stub: bytes
  `4a 4f 42 43 52 54 20 56 65 72 31 33` at LS offset `0x30`.  Encoders must
  not synthesize this marker; it comes from the linked SPU runtime.
- `__bss_start` and `_end` define the LS extent; the extent must be
  16-byte aligned.

## Local-Store Image

The encoder first creates a zero-filled byte array with length:

```text
ls_size = align16(max(p_paddr + p_memsz) across PT_LOAD segments)
```

For each `PT_LOAD`, it copies `p_filesz` bytes from the SPU ELF to
`image[p_paddr..p_paddr+p_filesz]` and leaves `p_memsz - p_filesz` as zero
fill.  This PT_LOAD image is not embedded directly for JQ jobbin2.  The encoder
applies the JQ runtime metadata patches below, then embeds the patched image at
jobbin2 blob offset `0x100`.

## JQ Runtime Metadata Patches

After materializing the LS image from the SPU ELF's `PT_LOAD` segments, the
encoder applies the following byte-level patches before embedding the image at
jobbin2 blob offset `0x100`.  These bytes are not emitted by the SPU runtime
stub for the `e_flags = 2` with-CRT case and must be supplied by the encoder.

| LS offset | Bytes | Value | Purpose |
|---:|---:|---|---|
| `0x20..0x23` | 4 | `62 69 6e 32` (`bin2`) | The JM2 dispatcher reads LS `0x20` and rejects the descriptor unless this slot is `BIN2` or `bin2`. |
| `0x2e..0x2f` | 2 | `00 00` | Clears the tail of the `.before_text` `0xa5` fill region before the `JOBCRT Ver13` marker at LS `0x30`. |
| `0x50..0x53` | 4 | `ls_size` as BE `u32` | Overwrites 4 bytes of post-`_end` alignment padding in the SPU stub with `ls_size` as BE `u32`. Possibly a redundant size copy for runtime lookup; consumer purpose is still under investigation. |
| `0x54..0x5b` | 8 | `00 00 00 00 ff ff ff ff` | Reorders the SPU stub's filler and stop-sentinel pair (`0xffffffff` at LS `0x54`, `0x00000000` at LS `0x58`) into `(0x00000000, 0xffffffff)`. Both words are present in the SPU stub's link-time layout; the reorder shape itself is not yet explained. Consumer purpose is still under investigation. |

The flat `JOBBIN` form (`e_flags = 1`) emits the validator slots from the SPU
runtime stub at link time.  The jobbin2 encoder must not apply this JQ patch
set to `e_flags = 1` images.

## jobbin2 Blob Layout

| Offset | Size | Source | Significance |
|---:|---:|---|---|
| `0x000..0x033` | `0x34` | ELF32 header | Diagnostic/helper-visible.  Identifies the wrapper as ELF32/SPU and points at the program headers. |
| `0x034..0x093` | `0x60` | Three ELF32 program headers | Diagnostic/helper-visible.  Describes the payload and optional note data inside the blob. |
| `0x094..0x0ff` | `0x6c` | Zero padding | Must be zero. |
| `0x100..0x100+ls_size` | `ls_size` | Patched SPU LS image | Runtime-significant.  `CellSpursJobHeader.eaBinary` points here, not at blob offset `0`. |
| after `0x100+ls_size` | variable | Optional diagnostic data | Data such as `.note.spu_name` may be present in `.spu_image` but is excluded from `sizeBinary`. |
| section tail | variable | Alignment padding | Not part of any symbol size and not runtime-significant. |

## ELF32 Wrapper Header Fields

All integer fields are big-endian.

| Field | Value |
|---|---|
| `e_ident[0..3]` | `0x7f 'E' 'L' 'F'` |
| `EI_CLASS` | `ELFCLASS32` |
| `EI_DATA` | `ELFDATA2MSB` |
| `EI_VERSION` | `EV_CURRENT` |
| ABI/padding bytes | Zero |
| `e_type` | `ET_EXEC` |
| `e_machine` | `EM_SPU` |
| `e_version` | `EV_CURRENT` |
| `e_entry` | Input SPU ELF `e_entry`; must equal `_start` |
| `e_phoff` | `0x34` |
| `e_shoff` | `0` in the embedded `.spu_image` wrapper.  A standalone diagnostic sidecar may carry section headers; they are not part of the runtime contract. |
| `e_flags` | Input SPU ELF `e_flags` |
| `e_ehsize` | `0x34` |
| `e_phentsize` | `0x20` |
| `e_phnum` | `3` |
| `e_shentsize` | `0x28` |
| `e_shnum` | `0` in the embedded `.spu_image` wrapper |
| `e_shstrndx` | `0` in the embedded `.spu_image` wrapper |

## Program Headers

The wrapper emits three ELF32 program headers.

### Runtime Image LOAD

- `p_type = PT_LOAD`
- `p_offset = 0x100`
- `p_vaddr = 0`
- `p_paddr = 0`
- `p_filesz = ls_size`
- `p_memsz = ls_size`
- `p_flags = PF_R | PF_X`
- `p_align = 0x80`

This is the range covered by `jobheader.eaBinary` and `sizeBinary`.

### Data LOAD Descriptor

- `p_type = PT_LOAD`
- `p_offset = 0x100 + data_paddr`
- `p_vaddr = data_paddr`
- `p_paddr = data_paddr`
- `p_filesz` and `p_memsz` mirror the input data `PT_LOAD` when present; zero
  is valid for JQ workloads with no materialized data segment.
- `p_flags = PF_R | PF_W`
- `p_align = 0x80`

This header preserves the SPU ELF's data-segment metadata for helper tooling.
The runtime payload is still governed by the runtime-image LOAD plus
`sizeBinary`.

### NOTE Descriptor

- `p_type = PT_NOTE`
- `p_offset = note blob offset when a note is emitted`, normally
  `0x100 + ls_size`
- `p_vaddr` and `p_paddr` are the note LS address from the input note placement
  when present.
- `p_filesz = note byte length`
- `p_memsz = 0`
- `p_flags = PF_R`
- `p_align = 0x10`

Note data is diagnostic and not included in `sizeBinary`.

## `.scespuversion` and `.debug_scespuversion`

These sections are not required by the jobbin2 runtime contract.  They are not
referenced by the jobheader relocation, are not included in the runtime LS image
range, and are not needed for CMake's section extraction path.  A independent
encoder should omit them unless a later consumer proves a concrete need.  The
tracked ABI is `.spu_image` plus `.spu_image.jobheader` plus the
symbols/relocation described below.

## PPU Object Contract

The PPU object must be ELF64/PowerPC relocatable and expose:

- `.spu_image`, alloc/read-only data, `0x80` alignment.
- `.spu_image.jobheader`, alloc/read-only data, `0x10` alignment, with
  relocation support.
- Global symbols:
  - `_binary_<name>_jobbin2_start`: object symbol at `.spu_image + 0`, size
    equal to the meaningful blob byte count, excluding trailing section
    padding.
  - `_binary_<name>_jobbin2_end`: `.spu_image + meaningful_blob_byte_count`.
  - `_binary_<name>_jobbin2_size`: ABS value equal to the meaningful blob byte
    count.
  - `_binary_<name>_jobbin2_jobheader`: object symbol at
    `.spu_image.jobheader + 0`, size `0x30`.

The meaningful blob byte count is `0x100 + ls_size` plus any diagnostic note
bytes included before the blob end.  It excludes alignment padding added only to
satisfy `.spu_image` section alignment.

## Jobheader Template

The jobheader template is exactly `0x30` bytes.  It serializes the named fields
of `CellSpursJobHeader` through `jobType/reserved3`.  The public C type has
`sizeof(CellSpursJobHeader) == 0x40` because the structure has 16-byte
alignment; bytes `0x30..0x3f` are tail padding, not header fields, and are not
serialized in `.spu_image.jobheader`.

Consumers should copy the `0x30`-byte template into a zero-initialized
`CellSpursJobHeader` or `CellSpursJob*` descriptor.  The tail padding in the
public C object must not carry semantic data.

Template fields, all big-endian:

| Offset | Field | Value |
|---:|---|---|
| `0x00` | `eaBinary` high word | Zero.  The current jobbin2 object contract assumes the blob effective address fits in 32 bits. |
| `0x04` | `eaBinary` low word | Template addend for the runtime payload address.  In a direct-linked `.ppu.o`, this word carries an `R_PPC64_ADDR32` relocation against `_binary_<name>_jobbin2_start + 0x100`.  In an extracted raw sidecar, the word value is `0x100`; PPU code adds the actual blob EA at runtime. |
| `0x08` | `sizeBinary` | `ls_size` in 16-byte units, `(ls_size + 15) >> 4`.  Covers the LS image starting at `blob + 0x100` and excludes the prefix, note data, and section padding. |
| `0x0a` | `sizeDmaList` | Initial DMA-list size; zero for the current JQ wrapper output. |
| `0x0c` | `eaHighInput` | Zero unless the job descriptor uses high EA input-list state. |
| `0x10` | `useInOutBuffer` | Zero for input-buffer mode. |
| `0x14` | `sizeInOrInOut` | Zero in the template; per-job code may fill it. |
| `0x18` | `sizeOut` | Zero in the template; per-job code may fill it. |
| `0x1c` | `sizeStack` | Zero in the template; JQ runtime derives stack handling from the wrapped job metadata. |
| `0x1e` | `sizeScratch/sizeHeap` | Zero in the template. |
| `0x20` | `eaHighCache` | Zero in the template. |
| `0x24` | `sizeCacheDmaList` | Zero in the template. |
| `0x28` | `idJob/reserved2` | Zero in the template. |
| `0x2c` | `jobType` | `CELL_SPURS_JOB_TYPE_BINARY2` (`4`). |
| `0x2d..0x2f` | `reserved3` | Zero. |

## Relocation

The PPU object carries one relocation for direct object consumers:

- Section: `.spu_image.jobheader`
- Offset: `0x04`
- Type: `R_PPC64_ADDR32`
- Symbol: `_binary_<name>_jobbin2_start`
- Addend: `0x100`

This relocation fills the low word of the big-endian `u64 eaBinary`.  The high
word remains zero for the current 32-bit EA model.  Raw sidecar extraction
strips the relocation, leaving the `0x100` addend as data.

## Example

For the `hello-spurs-jq` workload, `ls_size` is `0x880` bytes and `sizeBinary`
is `0x88` 16-byte units.  The meaningful blob size is `0x9d0` bytes when the
diagnostic note is included, while the `.spu_image` section may be padded to
`0xa00` for alignment.  These values illustrate the size relationships and are
not global constants.

## Coverage

This specification covers the JQ with-CRT job form (`e_flags = 2`) with a
single `PT_LOAD` code segment and at most a zero-size data segment.
Multi-segment data `PT_LOAD` shapes and other `e_flags` values are not yet
covered; encoders may reject unsupported shapes pending coverage expansion.
