# rsx-cg-compiler — REVERSE_ENGINEERING.md

Byte-level encoding notes for NV40 fragment and vertex program
instructions, derived from probing sce-cgc 475.001 output via wine.
The byte-diff harness at `tests/run_diff.sh` is the authoritative
gate for every encoding claim here.

**2026-05-01: 96/96 shaders match sce-cgc byte-for-byte.**

---

## Halfword swap

`FpAssembler` stores ucode words in **logical** layout, then
`words()` runs a halfword swap before disk write:
`halfswap(w) = (w >> 16) | (w << 16)`.  So an on-disk uint32
read little-endian (LE) is halfswapped to recover the logical
encoding.  The console output from `rsx-cg-compiler` (without
`--emit-container`) prints the on-disk hex words — same bytes
that end up in the `.fpo` ucode region.

### Mapping table (logical ↔ on-disk)

| Logical bits | On-disk bits | On-disk byte(s) |
|---|---|---|
| 0–15 | 16–31 | bytes 2–3 |
| 16–31 | 0–15 | bytes 0–1 |

Example: logical bit 24 (MSB of opcode) → on-disk bit 8 (byte 1).

---

## hw[0] field layout (logical)

| Bits   | Field | Notes |
|--------|-------|-------|
| 0      | PROGRAM_END | Set by `markEnd()` on last instruction |
| 1–6    | OUT_REG | Destination register index (mask 0x3F << 1) |
| 7      | OUT_REG_HALF | Set when dst.is_fp16 |
| 8      | COND_WRITE_ENABLE | cc_update — writes result to condition code |
| 9–12   | OUTMASK | X(9), Y(10), Z(11), W(12) |
| 13–16  | INPUT_SRC | Interpolated varying index (4 bits) |
| 17–20  | TEX_UNIT | Texture unit index (4 bits) |
| 22–23  | PRECISION | 0=FP32, 1=FP16, 2=FX12 |
| 24–29  | OPCODE | 6-bit opcode (mask 0x3F << 24) |
| 30     | OUT_NONE | Set for CC-write-only and KIL destinations |
| 31     | OUT_SAT | Saturate output |

---

## Opcodes (NV40 FP)

```
NOP=0x00  MOV=0x01  MUL=0x02  ADD=0x03  MAD=0x04
DP3=0x05  DP4=0x06  DST=0x07  MIN=0x08  MAX=0x09
SLT=0x0A  SGE=0x0B  SLE=0x0C  SGT=0x0D  SNE=0x0E
SEQ=0x0F  FRC=0x10  FLR=0x11  KIL=0x12  PK4B=0x13
UP4B=0x14 DDX=0x15  DDY=0x16  TEX=0x17  TXP=0x18
TXB=0x19  TXD=0x1A  TXL=0x1B  COS=0x22  SIN=0x23
RCP=0x20  RSQ=0x21  EX2=0x24  LG2=0x25  LIT=0x1C
```

---

## Condition codes

```
NVFX_COND_TR = 7  (always true)
NVFX_COND_LT = 1
NVFX_COND_EQ = 2
NVFX_COND_LE = 3
NVFX_COND_GT = 4
NVFX_COND_NE = 5
NVFX_COND_GE = 6
```

---

## CC-write sentinel (OUT_NONE + OUT_REG=0x3F)

When an instruction writes only to the condition code register
(no destination register), the destination is encoded as:
- type = NVFXSR_NONE → sets OUT_NONE (bit 30)
- index = 0x3F → sets OUT_REG = 0x3F (bits 1–6)

This sentinel pattern is used for:
- Comparison instructions with cc_update=1 (SETRC forms)
- KIL instructions
- MULXC (multiply extended condition code)

---

## hw[2] (SRC1) — FX12 comparison encoding

When a comparison instruction (SLT/SGT/SEQ/SLE/SNE, opcodes
0x0A–0x0F) uses precision=FX12 (2), sce-cgc sets additional
bits in hw[2] beyond the common SRC1 fields:

| Bits    | Value           | Field |
|---------|-----------------|-------|
| 0–17    | (common src)    | SRC1 common fields |
| 18      | 0               | SRC1_ABS |
| 19–20   | `01` (1)        | Input precision = FP16 |
| 21–27   | `0001011` (0x0B)| Unknown — constant 0x0B |
| 28–30   | (scale)         | DST_SCALE |
| 31      | 0               | IS_BRANCH |

On-disk, these bits produce `0x00020168` in word[2] after
halfswap (logical `0x01680002` → on-disk `0x00020168`).

The 0x01680000 portion in the logical word is:
`(1u << 19) | (0x0Bu << 21)`.

This encoding is applied only when:
- `insn.precision == NVFX_FP_PRECISION_FX12` (2)
- `opcode >= NVFX_FP_OP_OPCODE_SLT && opcode <= NVFX_FP_OP_OPCODE_SEQ`

SGE uses FP32 precision for discard comparisons, so it does NOT
carry this encoding (word[2] = 0x00020000 on-disk).

Discovered via probing: all FX12 comparison shaders (SLT, SGT,
SEQ, SLE, SNE) carry 0x0168 regardless of const value, discard
lane, or comparison count.  SGE uses FP32 and lacks the bits.

---

## registerCount (CgBinaryFragmentProgram field)

`registerCount` in the .fpo container is the allocated register
count in the NV40 register file.  sce-cgc semantics:

- **Minimum is 2** (R0 implicit + 1 spare), even when only
  R0 (result.color) is used.
- **fp16 H registers pack two per hw slot**: H0/H1 → R0,
  H2/H3 → R1, H4/H5 → R2, etc.  `registerCount` tracks hw
  slot count, NOT individual fp16 aliases.
- **Formula**: `registerCount = max(2, numTempRegs)` where
  `numTempRegs` is the count of allocated hw register slots.
  `emitDst` tracks this via `hwReg = dst.is_fp16 ? index>>1 : index`
  and `numTempRegs_ = max(numTempRegs_, hwReg + 1)`.

Examples:
- identity_f: R0 only → hwReg max=0 → numTempRegs=1 → regCount=2
- discard_tex_varying_f: R0 + H2 → H2→hwReg=1 → numTempRegs=2 → regCount=2
- uniform_branch_modify_f: R0 + R1 + H4 → H4→hwReg=2 → numTempRegs=3 → regCount=3

`FpAssembler::numTempRegs_` starts at 1 (R0).

---

## Discard implementation (probed against sce-cgc 475.001)

**Status: 96/96 byte-match.  All discard shaders match at
the full container level (header + parameters + strings +
program subtype + ucode).**

### Test shaders

- `tests/shaders/discard_tex_alpha_f.cg` — compound condition:
  `if (u_alphaDiscard > 0.0 && out_color.a < u_alphaDiscard) discard;`
  All fields byte-match sce-cgc.

- `tests/shaders/discard_tex_varying_f.cg` — simple condition with
  post-discard output mixing:
  `if (tex.a < 0.1) discard; out = float4(tex.xyz, varying * tex.a);`
  All fields byte-match sce-cgc.

### Container: `$kill_0000` synthetic parameter

Shaders that emit a KIL opcode (via `discard;`) get a synthetic
`$kill_0000` CgBinaryParameter appended after the user-visible
entries.  This is distinct from `#pragma alphakill` parameters
(which are indexed per-sampler).  The container emitter adds
this when `attrs.pixelKill` is set AND no pragma-based
`alphakillSamplers` exist (to avoid double-counting).

Parameter layout: `$kill_0000` uses type=CG_FLOAT4, res=CG_UNDEFINED
(0x0cb8), var=CG_VARYING, direction=CG_OUT, paramno=0xFFFFFFFF
(synthetic), isReferenced=0, no semantic string.

### Compound-condition: embedded uniform const offsets

When the RHS of a comparison is a uniform (e.g.,
`out_color.a < u_alphaDiscard`), the comparison's inline const
block is a zero-filled placeholder that the runtime patches with
the uniform's value.  The ucode byte offset of this const block
must be recorded in the `embeddedConstUcodeOffsets` vector so
the .fpo container can emit a `CgBinaryEmbeddedConstant` record.

Record at:
1. Compound pre-pass MOV to R1 (loads uniform) — `asm_.currentByteSize()` before the const block
2. RHS=Uniform comparison handlers (Varying, UniformScalar, TexResult) — same pattern

The record's ucode offsets are written in **descending order** in the
container (sce-cgc lists largest ucode offset first).

### Single-condition discard

For `if (cond) discard;` with a single comparison (no `&&`):

Reference shape:
```
SLTXC RC.x, R0_n.w, {lit}_n.x    # comparison with cc_update
KILR (NE.x)                       # kill if condition true
```

Our emit uses cc_update=1 on the comparison instruction,
writing directly to the condition code register (via the
CC-write sentinel).  The `_n` (negate) suffix on operands
is an NV40 ISA feature that sce-cgc uses; our equivalent
uses the un-negated comparison with the same logical sense.

### Compound-condition discard (LogicalAnd, `&&`)

For `if (a && b) discard;`:

Reference shape (e.g., discard_tex_alpha_f):
```
MOVR R1.x, u_alphaDiscard.x     # load uniform (pre-pass)
SLTR H2.w, R0, u_alphaDiscard.x # tex.a < uniform → H2.w
SGTR H2.x, R1, {0}.x            # uniform > 0.0 → H2.x
MULXC RC.x, H2, H2.w            # AND: H2.x & H2.w → RC.x
KILR (NE.x)                      # kill if RC.x != 0
```

Key points:
1. **Comparisons write to H2 lanes, NOT to CC directly.**
   Use the non-cc_update SETR forms (SLTR, SGTR, etc.) with
   destination H2.<lane>.  Do NOT set cc_update on the
   comparisons — that would write directly to CC, preventing
   the MULXC combine.

2. **Emission order matches LogicalAnd RHS-then-LHS.**
   sce-cgc emits the comparisons in reverse LogicalAnd order
   (RHS comparison first, then LHS).  We reverse the rcmps
   vector to match.

3. **Uniform loads in a pre-pass.**
   Any UniformScalar-LHS comparisons load their uniform into
   R1 via MOVR BEFORE any comparison instruction.  This matches
   sce-cgc's schedule where the MOVR precedes both comparisons.

4. **rhsKind=Uniform requires a zero const block.**
   When the RHS of a comparison is a uniform (e.g.,
   `out_color.a < u_alphaDiscard`), the comparison must
   reference a CONST inline block filled with zeros (the
   uniform's runtime-patched slot), NOT the rhsLiteral value.

5. **MULXC encoding.**
   The MULXC instruction uses:
   - Opcode: MUL (0x02)
   - cc_update = 1 (writes to CC)
   - Precision: FX12 (2) — sce-cgc encodes MULXC with
     precision=2, not FLOAT32.  This is critical for byte-match.
   - SRC0: H2 with default xyzw swizzle
   - SRC1: H2 with wwww swizzle (lane 3 broadcast)
   - DST: CC sentinel (OUT_NONE + OUT_REG=0x3F)
   - OUTMASK: X (only lane 0)

6. **KIL encoding.**
   The KIL instruction uses:
   - Opcode: KIL (0x12)
   - cc_cond: NE (5)
   - cc_swz: {0,0,0,0} (only lane x matters)
   - OUTMASK: X|Y (3) — NOT 0xF (ALL) and NOT 0x0.  sce-cgc
     sets mask X|Y for KIL; other values cause byte mismatch.
   - tex_unit: leave at -1 (unset) — translates to 0 in on-disk.
   - DST: CC sentinel

7. **TexResult comparison swizzle.**
   For TexResult-LHS comparisons, SRC0 uses the default xyzw
   swizzle (NOT smeared to the target lane).  The destination
   mask selects the correct lane.  sce-cgc encodes it this way;
   smearing the swizzle produces different (wrong) bytes.

8. **UniformScalar comparison swizzle.**
   For UniformScalar-LHS comparisons, SRC0 (R1) also uses
   default xyzw swizzle.  The RHS (CONST) is smeared to xxxx.

### If-conversion (Shape 5)

`nv40_if_convert.cpp` Shape 5 matches `if(cond){discard;}` patterns:
- Matches explicit branch or fallthrough
- Hoists discard into entry block
- Replaces CondBranch with unconditional Branch to join
- Inlines join block into entry when join has only entry as predecessor

---

## Comparison opcodes for discard

```
SGT=0x0D  SGE=0x0B  SLT=0x0A  SLE=0x0C  SEQ=0x0F  SNE=0x0E
```

For compound conditions, use the non-CC-write forms:
```
SLTR H2.w → opcode SLT(0x0A), dst H2, mask w-only, no cc_update
SGTR H2.x → opcode SGT(0x0D), dst H2, mask x-only, no cc_update
```

For simple (single-condition) discard, use CC-write forms:
```
SLTXC RC.x → opcode SLT(0x0A), dst CC-sentinel, cc_update=1
```

---

## VecConstruct with mixed tex-extract + Mul

**Status: Implemented.  discard_tex_varying_f passes.**

Shaders like `discard_tex_varying_f.cg` produce:
```cg
out = float4(tex.x, tex.y, tex.z, varying * tex.a);
```

This creates an IR VecConstruct with 4 operands:
- 3× ScalarExtract from the same tex result (identity lanes)
- 1× Mul(varying, ScalarExtract(tex, w))

The reference compiler emits:
```
MOVH H2.x, f[COL0]    # promote varying to H2
MOVR R0.xyz, R0       # identity: preserve tex xyz
MULR R0.w, R0, H2.x   # tex.w * varying → R0.w
```

The (varying, temp) Mul capture in the arithmetic handler
recognises this pattern and emits the MOVH + MULR sequence.
The VecConstruct→StoreOutput path decomposes the construct
into individual lane writes: identity MOVR for the tex-extract
lanes, and MULR.w for the Mul lane.
