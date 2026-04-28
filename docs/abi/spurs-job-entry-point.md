# SPURS Job entry-point ABI - normative specification

Binary contract for SPU job binaries that run under the SPURS job-chain
dispatcher (`libspurs.sprx`). Conformance is enforced at link time by the
toolchain and at runtime by the dispatcher.

---

## 1. SPU ELF identity

Every linked SPU job ELF MUST satisfy:

| Field          | Required value                          |
|----------------|-----------------------------------------|
| `EI_CLASS`     | `ELFCLASS32`                            |
| `EI_DATA`      | `ELFDATA2MSB` (big-endian)              |
| `e_machine`    | `EM_SPU` (23)                           |
| `e_type`       | `ET_EXEC`                               |
| `e_entry`      | `&_start` (must be nonzero)             |
| `e_flags`      | see § 1.1                               |

### 1.1 `e_flags` semantics

`e_flags` selects the SPURS program type. Stock SPU GCC emits `0`; the
patched toolchain sets it via the `-mspurs-*` driver flags listed below.

| Value | Program type             | GCC flag                    |
|-------|--------------------------|-----------------------------|
| 0     | (unspecified — emits warning at tooling time) | none |
| 1     | SPURS Job 2.0 w/o CRT    | `-mspurs-job`               |
| 2     | SPURS Job 2.0 with CRT   | `-mspurs-job-initialize`    |
| 3     | SPURS Task               | `-mspurs-task`              |
| ≥ 4   | invalid (rejected)       | —                           |

Bit semantics: bit 0 = job, bit 1 = job-with-CRT, both = task.

The "w/o CRT" form (`e_flags = 1`) is what the `JOBBIN` build path
produces and what this document specifies. The "with CRT" form
(`e_flags = 2`) additionally embeds a memory-check init/finalize pair
in `_start` and is not covered here.

---

## 2. Section layout

A linked SPURS Job ELF has two LOAD segments and one PT_NOTE
segment, laid out as follows:

```
LS offset    Section                Notes
---------    -------------          --------------------------------------
0x0000       .SpuGUID               16 bytes, AX (alloc + execute), align 1
0x0010       .before_text           _start trampoline (32 bytes; § 3)
0x0030       .text                  cellSpursJobMain2 + dependencies
…            .rodata                optional; readonly, in LOAD-code segment
0x????       .data                  vaddr 0x80-aligned (= LOAD-code end
                                    aligned up to 0x80)
…            (BSS)                  bracketed by __bss_start / _end
…            .note.spu_name         non-loadable PT_NOTE segment (§ 2.1)
```

`.bss` size MUST be a multiple of 16. `__bss_start` and `_end` are
required global symbols; the runtime's `cellSpursJobInitialize`
zero-fills `[__bss_start, _end)` if the with-CRT form is used.

**Phdr structure**: 3 program headers — LOAD code (RX), LOAD data
(RW), NOTE (R, non-loadable). The dispatcher does not parse the ELF
wrapper at runtime (see § 6.1); it DMAs raw bytes from `eaBinary`
straight into LS. The ELF wrapping is for the build-time job-image
extraction step, not for the runtime.

### 2.1 `.note.spu_name`

A standard ELF NOTE section in its own PT_NOTE program header (NOT
allocated, NOT in any LOAD segment). Format:

```
namesz = 8                      ; "SPUNAME\0"
descsz = 32                     ; padded basename of the binary
type   = 1
name   = "SPUNAME\0"
desc   = "<basename>.elf\0...\0"  ; null-padded to 32 bytes
```

This section carries diagnostic metadata only; the dispatcher does
not consult it. It is preserved in the linked ELF for debugger /
tooling consumption.

---

## 3. `_start` trampoline

The dispatcher branches to the binary's `_start` symbol with:

| Reg | Contents                                       |
|-----|------------------------------------------------|
| `$0` | dispatcher return address                      |
| `$1` | job-allocated stack-pointer top                |
| `$3` | `CellSpursJobContext2 *` (argument 1)          |
| `$4` | `CellSpursJob256 *` (argument 2)               |
| `$80..$127` | callee-saved; preserve bitwise across `_start` |

Canonical `_start` body — exactly 16 bytes (4 instructions); the
`.before_text` section is zero-padded out to its 32-byte alignment.
With `.SpuGUID` at vaddr 0..0xF and `.before_text` at vaddr 0x10..0x2F
the bytes are:

```
LS 0x10:  44 01 28 50  xori $80, $80, 4         ; toggle bit 2 of $80
LS 0x14:  32 00 00 80  br   _start+0x8           ; (forward no-op branch)
LS 0x18:  44 01 28 50  xori $80, $80, 4         ; toggle back -> net no-op
LS 0x1c:  32 00 ?? ??  br   cellSpursJobMain2    ; tail-jump (REL16)
LS 0x20..0x2f: 0x00 padding (alignment fill — no magic, no metadata)
```

The xori-pair toggles bit 2 of `$80` and toggles it back — net no-op
on the register. The dispatcher recognises this exact 4-instruction
byte signature as a valid job entry stub.

`cellSpursJobMain2` is responsible for normal stack-frame setup and
returns via `bi $0` to the dispatcher's per-job epilogue.

---

## 4. Required symbols

| Symbol                | Purpose                                       |
|-----------------------|-----------------------------------------------|
| `_start`              | binary entry point (must equal `e_entry`)     |
| `cellSpursJobMain2`   | user job entry called by `_start`             |
| `__bss_start`, `_end` | bracket the BSS clear range                   |

The user job MUST define `cellSpursJobMain2` with the C signature:

```c
void cellSpursJobMain2(CellSpursJobContext2 *jobContext,
                       CellSpursJob256      *job);
```

---

## 5. Position-independence

Job binaries SHOULD be linked with `-fpic -Wl,-q` (PIC code, retained
relocations) so the dispatcher can DMA-load them into LS at any
runtime offset. The xori-pair `_start` trampoline is PC-relative-only
and works at any load address; the runtime BSS-clear (when used)
computes a PIC slide (`ila link-addr` + `brsl +8` + `sf slide`)
before dereferencing `__bss_start` / `_end`.

---

## 6. Job descriptor (PPU-side)

The PPU populates a `CellSpursJob256` (or `Job64` / `Job128`) before
queueing it via the chain command list. Header layout, alignment, and
field semantics follow `<cell/spurs/job_descriptor.h>` and
`<cell/spurs/job_chain.h>`.

### 6.1 BINARY2 descriptor fields (`jobType = 4`)

For `CELL_SPURS_JOB_TYPE_BINARY2` (`header.jobType = 4`), the leading
fields of `CellSpursJobHeader` are populated as a flat job descriptor.
The `binaryInfo[10]` array is a UNION view of:

```c
struct {
    uint64_t eaBinary;                /* offset 0..7  */
    uint16_t sizeBinary __packed__;   /* offset 8..9  */
};
```

Set the union members directly:

```c
header.eaBinary   = (uint64_t)(uintptr_t)spu_image_blob;
header.sizeBinary = CELL_SPURS_GET_SIZE_BINARY(spu_image_byte_size);
header.jobType    = CELL_SPURS_JOB_TYPE_BINARY2;
```

`eaBinary` MUST be 16-byte-aligned. `sizeBinary` is the image size in
16-byte units (`(byte_size + 15) >> 4`).

`eaBinary` points directly at the flat SPU LS image — first 16 bytes
MUST be the `.SpuGUID` content, followed by `.before_text` (the
xori-pair entry stub) at offset 0x10. The dispatcher DMAs from
`eaBinary` straight into LS starting at LS 0; it does NOT parse an
ELF wrapper. The build-time pipeline therefore feeds the dispatcher a
flat raw image obtained by running `spu-elf-objcopy -O binary` on the
linked SPU ELF.

> **Note**: It is a common error to set `binaryInfo[0..3]` to an ASCII
> magic word (e.g. "bin2"). `binaryInfo[0..3]` is the high 32 bits of
> the 64-bit `eaBinary` field — stuffing it parks the EA in unmapped
> memory and the dispatcher's image DMA fails, surfacing as
> `CELL_SPURS_JOB_ERROR_MEMORY_SIZE (0x80410a17)` written to
> `CellSpursJobChain.error` (`+0x80`).

For `jobType == 0` (legacy non-BINARY2 path), the same union view
applies and there is no magic word at any offset. The
`CELL_SPURS_JOB_TYPE_BINARY2` form is the canonical path.

### 6.2 Image size cap

The dispatcher's per-job LS budget caps `sizeBinary` at `0x3FBFF`
bytes (= 256 KB − 1024 − 1) net of dispatcher overhead. A request
exceeding this halts the chain with
`CELL_SPURS_JOB_ERROR_MEMORY_SIZE (0x80410a17)`.

`sizeStack` and `sizeScratch` in `CellSpursJobHeader` are used by the
legacy (`jobType = 0`) path. For `jobType = 4` the dispatcher
provisions stack/scratch from the chain's per-job budget configured
via `cellSpursJobChainAttributeInitialize(... sizeJobDescriptor ...)`,
and these descriptor fields SHOULD be left zero.

---

## 7. Build pipeline

The `JOBBIN` flag on `ps3_add_spu_image` (CMake helper in
`cmake/ps3-self.cmake`) implements the full pipeline:

1. Compile SPU sources with the SPU GCC.
2. Link with `-mspurs-job -fpic -Wl,-q -nostartfiles -T spurs_job.ld`,
   pulling in `libspurs_job` for the `_start` trampoline + BSS clear.
3. Run `spu-elf-objcopy -O binary` on the linked ELF to extract a
   flat LS image (sized exactly to cover both LOAD segments).
4. Run `bin2s` on the flat image to embed it as a `.rodata` blob in
   the PPU executable, exposing `<NAME>_bin` (data pointer) and
   `<NAME>_bin_size` (byte count) symbols.
5. PPU code populates the `CellSpursJob256` descriptor with
   `eaBinary = <NAME>_bin`, `sizeBinary = (<NAME>_bin_size + 15) / 16`,
   `jobType = CELL_SPURS_JOB_TYPE_BINARY2`.
