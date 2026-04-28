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
| `e_entry`      | `0x00000000` (must equal `_start` addr) |
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

Until the matching SPU GCC patch lands in our toolchain, samples can
patch `e_flags=1` into the linked ELF post-link with `dd of=… seek=36
count=4 conv=notrunc` on the byte stream. The runtime accepts the
resulting binary unchanged.

---

## 2. Section layout

A linked SPURS Job ELF MUST contain exactly one LOAD segment laid out as:

```
LS offset    Section            Required
---------    -------------      ----------------------------------
0x0000       .before_text       _start trampoline (32 bytes; § 3)
0x0020       .text              cellSpursJobMain2 + dependencies
…            .rodata, .data     optional; aligned 16
…            (BSS)              bracketed by __bss_start / _end
```

Each section MUST end on a 16-byte boundary. The complete loadable image
size MUST equal `sizeBinary << 4` exactly (no trailing garbage in the
last 16-byte chunk).

`.bss` size MUST be a multiple of 16. `__bss_start` and `_end` are
required global symbols; the runtime's `cellSpursJobInitialize`
zero-fills the range `[__bss_start, _end)` if the with-CRT form is used.

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

Canonical `_start` body (16 bytes of code; `.before_text` padded to 32
bytes total with trailing `nop $0`):

```
00:  44 01 28 50  xori $80, $80, 4         ; toggle bit 2 of $80
04:  32 00 00 80  br   _start+0x8           ; (forward no-op branch)
08:  44 01 28 50  xori $80, $80, 4         ; toggle back -> net no-op
0c:  32 00 ?? ??  br   cellSpursJobMain2    ; tail-jump (REL16)
10..1f: 40 20 00 00  nop $0                 ; (4 nops, padding)
```

The xori-pair toggles bit 2 of `$80` and toggles it back — net no-op on
the register. The dispatcher recognises this exact byte signature as a
valid job entry stub.

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
relocations) so the dispatcher can DMA-load them into LS at any runtime
offset. The xori-pair `_start` trampoline is PC-relative-only and works
at any load address; the runtime BSS-clear (when used) computes a PIC
slide (`ila link-addr` + `brsl +8` + `sf slide`) before dereferencing
`__bss_start` / `_end`.

---

## 6. Job descriptor (PPU-side)

The PPU populates a `CellSpursJob256` (or `Job64` / `Job128`) before
queueing it via the chain command list. Header layout, alignment, and
field semantics follow `<cell/spurs/job_descriptor.h>` and
`<cell/spurs/job_chain.h>`.

`header.eaBinary` MUST be 16-byte-aligned. `header.sizeBinary` is the
loaded image size in 16-byte units (`(byte_size + 15) >> 4`).

> **Status (2026-04-27):** the exact `binaryInfo[10]` byte layout the
> dispatcher consumes for `jobType & CELL_SPURS_JOB_TYPE_BINARY2`
> (`jobType = 4`) is not yet documented here. The PPU-side
> `_cellSpursCheckJob` view (`bytes 0..3 = extra-size, 4..7 = 32-bit
> eaBinary, 8..9 = sizeBinary`) lets a hand-laid descriptor through
> PPU validation, but the runtime then applies an additional internal
> check that returns `CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR (0x80410a0b)`
> for any layout we've tried — the runtime appears to read
> `binaryInfo[10]` from a different field projection than the union
> view, or cross-references it against metadata our tooling does not
> yet emit.
>
> Until that's resolved, end-to-end jobbin2 dispatch goes through the
> `cellSpursJobHeaderSetJobbin2Param` helper consuming a tool-wrapped
> binary. Header surface, `_start` trampoline, linker script, and
> `JOBBIN` cmake flag are all in place; only the descriptor encoding
> step is gated.
