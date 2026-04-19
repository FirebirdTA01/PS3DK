# sce-cgc Reverse-Engineering Log

Running log of discoveries about Sony's Cg compiler (`sce-cgc.exe`) and
the resulting `CgBinaryProgram` (.vpo / .fpo) binary format.  Every time
we figure something out by experiment, it goes here — so the compiler
implementation doesn't have to re-derive facts and future-us can
trace back why the code looks the way it does.

Maintained by `rsx-cg-compiler`.  The byte-exact diff harness at
`tests/run_diff.sh` is the authoritative oracle; this doc captures
the *reasoning* behind the bits it checks.

---

## Tooling on the reference side

- `wine sce-cgc.exe --profile sce_{vp,fp}_rsx -o out.vpo foo.cg`
  — Sony's compiler.  `--profile sce_vp_rsx` is vertex program, `sce_fp_rsx`
  is fragment.  Mount path: `~/cell-sdk/475.001/cell/host-win32/Cg/bin/`.
- `wine sce-cgcdisasm.exe out.vpo`
  — Disassembler.  Prints human-readable `MOV o[0], v[0]` etc., plus
  parameter index / name / semantic / `resIndex` for every CgBinaryParameter.
  **This is the fastest way to figure out what sce-cgc's bits mean** —
  compile a test shader, disasm it, then cross-reference the ucode.
- `xxd out.vpo` — raw byte dump; correlate with the `CgBinaryProgram`
  layout in `reference/sony-sdk/.../Cg/cgBinary.h`.
- RPCS3 → "Create RSX Capture" on a running homebrew — captures the
  GPU command stream (ucode uploads, state bits) as a `.rrc` file you
  can re-open in RPCS3 to step through.  Useful for verifying runtime
  behavior (ucode diff after binding, constant-patching visibility,
  etc.) rather than compiler output.

---

## CgBinaryProgram header layout

From `Cg/cgBinary.h` (Sony SDK):

| Offset | Field                 | Notes |
|-------:|:----------------------|:------|
| 0x00   | `profile` (CGprofile) | 0x1b5b = `sce_vp_rsx` |
| 0x04   | `binaryFormatRevision`| 6 (CG_BINARY_FORMAT_REVISION) |
| 0x08   | `totalSize`           | includes the header itself |
| 0x0C   | `parameterCount`      |  |
| 0x10   | `parameterArray`      | offset to first CgBinaryParameter (usually 0x20) |
| 0x14   | `program`             | offset to CgBinaryVertexProgram / CgBinaryFragmentProgram |
| 0x18   | `ucodeSize`           | bytes (4 × instructionCount for VP) |
| 0x1C   | `ucode`               | offset to raw instruction blob |

All offsets are relative to the start of the `CgBinaryProgram`
struct (i.e. the start of the file for a standalone `.vpo`).

---

## CgBinaryParameter layout (48 bytes)

| Offset | Field              | Notes |
|-------:|:-------------------|:------|
| 0x00   | `type` (CGtype)    | 0x418 = `CG_FLOAT4`, 0x428 = `CG_FLOAT4x4` (observed) |
| 0x04   | `res` (CGresource) | 0x841 = ATTR0, 0x844 = ATTR3, 0x882 = C (const bank), 0x8c3 = HPOS, 0x8c5 = COL0 (observed) |
| 0x08   | `var` (CGenum)     | 0x1005 = varying, 0x1006 = uniform (observed) |
| 0x0C   | `resIndex`         | **absolute** register index (see allocation below); 0xFFFFFFFF for non-allocated |
| 0x10   | `name`             | string offset (rel. to file start); strings are NUL-terminated in the data region |
| 0x14   | `defaultValue`     | |
| 0x18   | `embeddedConst`    | |
| 0x1C   | `semantic`         | string offset; 0 if no semantic |
| 0x20   | `direction`        | 0x1001 = `CG_IN`, 0x1002 = `CG_OUT` (observed) |
| 0x24   | `paramno`          | user parameter number; all fields of a struct/matrix share the same number |
| 0x28   | `isReferenced`     | `CG_TRUE` (1) if used |
| 0x2C   | `isShared`         |  |

---

## Const-register allocation (profile sce_vp_rsx)

**Rules**, based on shaders compiled by sce-cgc and cross-checked
against sce-cgcdisasm output:

- `uniform float4x4` matrices: **bottom-up from c[256]**, 4 consecutive slots.
  - First matrix: c[256..259].  Second: c[260..263].
- `uniform float4` (and presumably lower-vector types): **top-down from c[467]** in declaration order.
  - First: c[467].  Second: c[466].  Third: c[465].  Etc.
- User const space is c[256..467] — **212 registers**.  c[0..255] is reserved.

This is NOT a first-fit allocator — types use disjoint regions:
matrices grow up from 256, scalars/vectors grow down from 467.  The
two regions meet somewhere in the middle if a shader exhausts them.

**Ucode encoding** of CONST_SRC:

- Field is `NV40_VP_INST_CONST_SRC` — **9 bits** in `hw[1]` at shift 12 (mask `0x1FF << 12`, bits 12..20).
- The PSL1GHT-ported `nv40_vertprog.h` defines the mask as `0xFF << 12` — that's wrong for
  sce-cgc-compatible output.  Bit 20 is part of the same field.
- Value = **absolute c[] register index**, no bias.  c[467] → `CONST_SRC=0x1d3`.
- Parameter struct stores the same absolute index in `resIndex`; sce-cgcdisasm prints it as `c[N]`.
- Verified by running rsx-cg-compiler against sce-cgc on `const_out_v.cg` — 9-bit encoding
  produces byte-exact output.

### Evidence

```
shader                 | uniform      | disasm | resIndex | 9-bit CONST_SRC
-----------------------|--------------|--------|----------|----------------
const_out_v.cg         | float4 col   | c[467] | 0x01d3   | 0x1d3 = 467
two_vec4_v.cg          | float4 col_a | c[467] | 0x01d3   | 0x1d3 = 467
two_vec4_v.cg          | float4 col_b | c[466] | 0x01d2   | 0x1d2 = 466
mvp_passthrough_v.cg   | float4x4 mvp | c[256] | 0x0100   | 0x100 = 256 (row 0)
mvp_passthrough_v.cg   | mvp[3]       | c[259] | 0x0103   | 0x103 = 259
```

## Single-const-per-instruction constraint

NV40 VP instructions can reference at most **one** const-bank source.
`a + b` where both `a` and `b` are uniforms is lowered by sce-cgc to:

```
MOV R0, c[b]
ADD dst, c[a], R0
```

Observed in `two_vec4_v.cg` / `add_uniforms_v.cg` disassembly:

```
MOV o[0], v[0];
MOV R0, c[466];        // preload col_b into temp
ADD o[1], c[467], R0;  // col_a + R0 — one const, one temp
END
```

Our `rsx-cg-compiler` IR-to-NV40 lowering honours this in stage 3c
by inserting a MOV-to-temp before any arithmetic op whose two
operands both come from the const bank.

## VEC opcode slot map

Different VEC-slot opcodes read operands from different source positions.
The `nvfx_insn::src[0..2]` array holds the per-operand register / swizzle
records, and a given opcode reads a specific subset:

| opcode | src slots used |
|:-------|:---------------|
| MOV    | `src[0]`                |
| ADD    | `src[0]`, `src[2]`      |
| SUB    | `src[0]`, `src[2]` (src[2] negated) |
| MUL    | `src[0]`, `src[1]`      |
| MAD    | `src[0]`, `src[1]`, `src[2]` |
| DP3/DP4| `src[0]`, `src[1]`      |

(Confirmed against PSL1GHT `tools/cgcomp/source/vpparser.cpp`'s
per-opcode `{0, 2, -1}` etc. spec and sce-cgc byte output for
`add_uniforms_v.cg` — SRC1 of an ADD decodes as the default INPUT
filler, not the second operand.)

## Program header (CgBinaryVertexProgram, 6 u32)

Observed fields (match `cgBinary.h`):

- `instructionCount` — total NV40 VP instructions.
- `instructionSlot`  — load address; non-zero enables indexed reads.
- `registerCount`    — R register high-water-mark + 1.  `1` for a single-temp program.
- `attributeInputMask`  — bitmap of used `v[N]` input attributes (ATTR0 = bit 0).
- `attributeOutputMask` — bitmap of written outputs, in `SET_VERTEX_ATTRIB_OUTPUT_MASK` layout.
- `userClipMask`     — clip-plane enables (SET_USER_CLIP_PLANE_CONTROL bits).

`attributeOutputMask` uses method-specific bits.  From `sce-cgcdisasm`
output: a shader that writes HPOS + COL0 prints `FrontDiffuse`; we
haven't fully enumerated the bit list yet.

## `mul(matrix, vector)` lowers to 4 DP4s — reverse component order

Observed on `mvp_passthrough_v.cg`.  `mul(mvp, in_position)` where `mvp` is
a `float4x4` uniform lowers to four vector DP4 instructions, each writing
one destination component:

```
DP4 o[0].w, v[0], c[mvp+3];
DP4 o[0].z, v[0], c[mvp+2];
DP4 o[0].y, v[0], c[mvp+1];
DP4 o[0].x, v[0], c[mvp+0];
```

The order is **W, Z, Y, X** with matrix rows **base+3, base+2, base+1, base+0**.
There is no temp register — sce-cgc writes directly to the output.

## StoreOutput emit ordering: simple first, multi-insn last

When a shader has multiple outputs, sce-cgc does NOT emit them in
IR / source-code order.  Single-instruction StoreOutputs are emitted
first; multi-instruction ones (matrix×vector in particular, the 4-DP4
chain for HPOS) come at the end.  This lets the `NVFX_VP_INST_LAST`
flag land on the tail of the longest chain, which is the conventional
exit for a vertex program.

In `mvp_passthrough_v.cg` the Cg source reads:

```
out_position = mul(mvp, in_position);  // stout %_ POSITION
out_color    = in_color;               // stout %_ COLOR
```

IR order is POSITION-then-COLOR, but sce-cgc emits:

```
MOV o[1], v[3];              # out_color — 1 insn, emitted first
DP4 o[0].w, v[0], c[259];    # out_position begins
DP4 o[0].z, v[0], c[258];
DP4 o[0].y, v[0], c[257];
DP4 o[0].x, v[0], c[256];    # LAST flag here
```

`rsx-cg-compiler` queues matvecmul-consumed StoreOutputs into a
deferred list at IR walk, emits the simple ones first, then flushes
the deferred list.

## Fragment profile (sce_fp_rsx) — ucode on-disk layout

Reference capture from `identity_f.cg` (single-instruction passthrough:
`MOVR R0, f[COL0]`):

```
ucode (16 bytes, 1 instruction):
  3e010100 c8011c9d c8000001 c8003fe1
```

NV40 FP ucode is stored **halfword-swapped** on disk relative to how
the GPU / RSX FP-engine decodes the instruction fields.  Each u32 has
its two 16-bit halves swapped.  To read the "logical" word that maps
onto the bit layout in `nvfx_shader.h` (`NVFX_FP_OP_*`):

```
logical_word = (on_disk_word >> 16) | (on_disk_word << 16)
```

Applied to the identity FP:

| on-disk   | logical    | decoded (from `NVFX_FP_OP_*` layout) |
|:----------|:-----------|:--------------------------------------|
| 0x3e010100 | 0x01003e01 | opcode=MOV(0x01), OUT_REG=R0, OUTMASK=0xF, INPUT_SRC=COL0, PROGRAM_END |
| 0xc8011c9d | 0x1c9dc801 | source-0 operand encoding            |
| 0xc8000001 | 0x0001c800 | source-1 operand (unused default)    |
| 0xc8003fe1 | 0x3fe1c800 | source-2 operand (unused default)    |

Bit layout (after halfword-swap) matches `NVFX_FP_OP_OPCODE_SHIFT=24`,
`OUT_REG_SHIFT=1`, `OUTMASK_SHIFT=9`, `INPUT_SRC_SHIFT=13`,
`PROGRAM_END=bit 0`.

Implementation note for `rsx-cg-compiler`: emit instruction bytes in
logical form, then halfword-swap at the last step before writing to
the `.fpo` ucode blob.  Byte-exact match against sce-cgc is the gate.

### Source-operand encoding (identity_f.cg ground truth)

Cross-referenced sce-cgc's bytes against PSL1GHT cgcomp's
`emit_src` (compilerfp.cpp) and decoded each src word:

| logical word    | role                               | meaning |
|:----------------|:-----------------------------------|:--------|
| `hw[1] = 0x1c9dc801` | SRC0 = INPUT bank, COL0       | TYPE_INPUT(1) at bits 0-1; identity swizzle (XYZW) at bits 9-16; cc_cond=NVFX_COND_TR(7) at bits 18-20; identity cond_swz (X,Y,Z,W) at bits 21-28 |
| `hw[2] = 0x0001c800` | SRC1 = NONE (unused default)  | TYPE_TEMP(0) + identity swizzle.  All other bits clear. |
| `hw[3] = 0x3fe1c800` | SRC2 = NONE + ADDR_INDEX default | TYPE_TEMP(0) + identity swizzle, plus the high bits `0x7fc << NV40_FP_OP_ADDR_INDEX_SHIFT` (= `0x3fe00000`). |

The high bits in `hw[3]` are key: PSL1GHT cgcomp only writes
`(0x7fc << NV40_FP_OP_ADDR_INDEX_SHIFT)` when the input source is a
TC (texcoord) varying — gating perspective-correct interpolation.
**sce-cgc applies the same mask whenever ANY varying input is read**,
including COL0/COL1/POSITION/etc.  The bits are inert when the
INDEX_INPUT bit (1<<30) isn't set, so this is a "default safe value"
sce-cgc emits unconditionally for varying reads.

Our `FpAssembler::emitSrc` writes the mask in the `NVFXSR_INPUT`
arm, matching sce-cgc.  If a future shader is byte-exact with sce-cgc
*without* the mask in some case, the rule will need narrowing.

### TEX opcode (sampler2D `tex2D(tex, uv)` → `TEXR R0, f[TEX0], TEX0`)

Captured from `texture_sample_f.cg`:

```
ucode (16 bytes, 1 instruction):
  9e011700 c8011c9d c8000001 c8003fe1   (on disk)
  17009e01 1c9dc801 0001c800 3fe1c800   (logical, after halfword swap)
```

Decoded against `NVFX_FP_OP_*`:

| field         | bits   | value | meaning                        |
|:--------------|:-------|------:|:-------------------------------|
| PROGRAM_END   | 0      | 1     | last instruction               |
| OUT_REG       | 1-6    | 0     | R0 (= result.color)            |
| OUT_REG_HALF  | 7      | 0     | fp32 result (TEX**R**)         |
| OUTMASK       | 9-12   | 0xF   | xyzw                           |
| INPUT_SRC     | 13-16  | 4     | TC0 (NVFX_FP_OP_INPUT_SRC_TC0) |
| TEX_UNIT      | 17-20  | 0     | sampler bound to tex unit 0    |
| OPCODE        | 24-29  | 0x17  | TEX (NVFX_FP_OP_OPCODE_TEX)    |

The SRC0 / SRC1 / SRC2 words are bit-identical to `identity_f`'s.
The TEX instruction is essentially "MOV with opcode swapped + TC0
input + TEX_UNIT field" — no new source-operand encoding to learn.

**Sampler → tex-unit binding:** sce-cgc assigns texture units in
declaration order starting from 0.  Single sampler `tex` →
`TEXUNIT0` (visible in disasm: `#1: sampler2D: tex: : TEXUNIT0`).
Our `lowerFragmentProgram` mirrors this with a `nextTexUnit`
counter that bumps on each `Uniform` parameter whose IRType is
`Sampler2D` or `SamplerCube`.

## .vpo container layout (verified 2026-04-18)

Same overall structure as .fpo (header + parameter table + strings +
program subtype + ucode, all big-endian, 16-byte aligned sections).
Differences from FP, all confirmed against `const_out_v.vpo`,
`add_uniforms_v.vpo`, `mvp_passthrough_v.vpo`, and `probe_tc_v.vpo`:

### CgBinaryVertexProgram fields (cgBinary.h, 24 bytes content)

| offset | size | field                  | notes                                    |
|-------:|-----:|:-----------------------|:-----------------------------------------|
|  0     | 4    | instructionCount       | total NV40 VP instructions               |
|  4     | 4    | instructionSlot        | load address; non-zero enables indexed reads (sce-cgc default 0) |
|  8     | 4    | registerCount          | R-register count; sce-cgc minimum is 1   |
| 12     | 4    | attributeInputMask     | bit n iff `v[n]` is read                 |
| 16     | 4    | attributeOutputMask    | front-face only (HPOS = no bit, COL0 = bit 0, TC0 = bit 14, ...) |
| 20     | 4    | userClipMask           | clip plane enables (sce-cgc default 0)   |

### attributeOutputMask: front-face only, HPOS implicit

Decoded against the `# attributeOutputMask` line in
`sce-cgcdisasm` output:

```
identity_v       (HPOS only)        → 0x0000  ("nothing" — HPOS implicit)
identity_color_v (HPOS + COL0)      → 0x0001  ("FrontDiffuse")
mvp_passthrough_v(HPOS + COL0)      → 0x0001
probe_tc_v       (HPOS + TC0)       → 0x4000
```

Bit layout: bit 0 = COL0 (FrontDiffuse), bit 1 = COL1 (FrontSpecular),
bit 2 = BFC0 (BackDiffuse), bit 3 = BFC1 (BackSpecular), bit 4 = FOGC,
bit 5 = PSZ, bits 14-21 = TC0..TC7.  HPOS contributes nothing.

PSL1GHT cgcomp also sets the back-face bit when COL0/COL1 is written
(`outputMask |= (4 << ...)`), making it `0x0005` for COL0.  sce-cgc
does NOT — only the front-face bit.  Our `VpAssembler` follows
sce-cgc.

### VP semantic → CGresource mapping

Inputs use NVIDIA's auto-bind to vertex attributes (CG_ATTR0..ATTR15):

| Cg semantic    | CGresource enum | numeric        |
|:---------------|:----------------|---------------:|
| `: POSITION`   | CG_ATTR0        | 2113 (0x0841)  |
| `: BLENDWEIGHT`| CG_ATTR1        | 2114 (0x0842)  |
| `: NORMAL`     | CG_ATTR2        | 2115 (0x0843)  |
| `: COLOR`      | CG_ATTR3        | 2116 (0x0844)  |
| `: COLOR1`     | CG_ATTR4        | 2117 (0x0845)  |
| `: TEXCOORDn`  | CG_ATTR(8+n)    | 2121+n (0x0848+n)|

Outputs use the post-rasteriser bank (CG_HPOS, CG_COL0, CG_TEX0+n):

| Cg semantic    | CGresource enum | numeric        |
|:---------------|:----------------|---------------:|
| out `: POSITION` / `: HPOS` | CG_HPOS  | 2243 (0x08c3) |
| out `: COLOR` / `: COL0`    | CG_COL0  | 2245 (0x08c5) |
| out `: COLOR1`              | CG_COL1  | 2246 (0x08c6) |
| out `: TEXCOORDn` / `: TEXn`| CG_TEX0+n | 2179+n (0x0883+n) |

(Note: VP `out : TEXCOORDn` uses CG_TEX0+n = 0x0883+n, NOT
CG_TEXCOORD0+n = 0x0c94+n which is the FP input variant.  Different
resource class for different roles.)

Uniforms land in the const bank — single resource code:

| param kind                  | CGresource | numeric       | resIndex                       |
|:----------------------------|:-----------|--------------:|:--------------------------------|
| `uniform float4 / scalar`   | CG_C       | 2178 (0x0882) | absolute c[N] (top-down from 467) |
| `uniform float4x4`          | CG_C       | 2178 (0x0882) | first row's c[N] (bottom-up from 256) |

### Matrix uniforms expand to parent + N rows

A `uniform float4x4 mvp` becomes 5 CgBinaryParameter entries in the
table:

| index | type      | name      | resIndex |
|------:|:----------|:----------|---------:|
| n     | CG_FLOAT4x4 (0x428) | `mvp`     | base    |
| n+1   | CG_FLOAT4   (0x418) | `mvp[0]`  | base    |
| n+2   | CG_FLOAT4   (0x418) | `mvp[1]`  | base+1  |
| n+3   | CG_FLOAT4   (0x418) | `mvp[2]`  | base+2  |
| n+4   | CG_FLOAT4   (0x418) | `mvp[3]`  | base+3  |

All five share the same `paramno` (the user-visible parameter index).
Confirmed: 688-byte `mvp_passthrough_v.vpo` byte-matches when emitted
this way (verified end-to-end via `tests/run_diff.sh`).

### Struct parameters — container naming rules

When a VP entry-point takes a struct (e.g. `VOUT main(VIN input)`),
sce-cgc names the synthesized field params:

| field role      | name format                        |
|:----------------|:-----------------------------------|
| input field     | `<struct-param-instance>.<field>`  |
| output field    | `<entry-function-name>.<field>`    |

Probed 2026-04-18 with `myStruct` as the input name and `o` vs `rt`
as the output local name: inputs use the user's struct-instance name
(so `myStruct.pos`), outputs always use the function name (so
`main.pos` regardless of `o` vs `rt`).

Additional rules for the `CgBinaryParameter.paramno` field:

- All input struct fields share the same `paramno` — the user param
  number of the struct parameter (typically 0, the only entry param).
- Output struct fields have `paramno = 0xFFFFFFFF` — they're
  synthesised from the `return` statement, not a user parameter.

Our IR builder now annotates each `LoadAttribute` with
`structParamName + fieldName` (from the source-level `input.pos`)
and each `StoreOutput` with `fieldName` (the output prefix is
recovered by the container emitter from the entry function name).
See `IRInstruction::structParamName` / `fieldName` in
`tools/rsx-cg-compiler/src/donor/ir/ir.h`.

The container emitter walks the IR's `LoadAttribute` / `StoreOutput`
instructions to build the parameter table for struct-flattened
entry points.  Verified byte-exact on `identity_v.cg` (224 bytes)
and `identity_color_v.cg` (368 bytes).

## .fpo container layout (verified 2026-04-18)

Reading SDK 475.001's `Cg/cgBinary.h` for the struct definitions and
cross-checking against `identity_f.fpo` and `tex_f.fpo` from sce-cgc:

```
+------------------------------------------------------------+
| 0x00  CgBinaryProgram header (32 bytes, big-endian)        |
|       profile / revision / totalSize / parameterCount /    |
|       parameterArray / program / ucodeSize / ucode         |
+------------------------------------------------------------+
| 0x20  CgBinaryParameter[parameterCount] (48 bytes each)    |
+------------------------------------------------------------+
| <X>   strings region (per-param: semantic\0 then name\0)   |
|       padded with NULs to a 16-byte boundary               |
+------------------------------------------------------------+
| <Y>   CgBinaryFragmentProgram subtype (22 bytes content    |
|       + padding to a 16-byte boundary)                     |
+------------------------------------------------------------+
| <Z>   ucode (16-byte aligned, halfword-swapped on disk)    |
+------------------------------------------------------------+
```

### CgBinaryFragmentProgram fields (cgBinary.h):

| offset | size | field                  | notes                                    |
|-------:|-----:|:-----------------------|:-----------------------------------------|
|  0     | 4    | instructionCount       | total NV40 FP instructions               |
|  4     | 4    | attributeInputMask     | SET_VERTEX_ATTRIB_OUTPUT_MASK bits — bits 0+2 for COLOR (front+back diffuse), bit 14+n for TC*n* |
|  8     | 4    | partialTexType         | 2 bits per texunit, 0=full load (default) |
| 12     | 2    | texCoordsInputMask     | bit n iff TEXCOORD*n* read                |
| 14     | 2    | texCoords2D            | bit n iff TEXCOORD*n* sampled as 2D — sce-cgc default is `0xFFFF` (all set) |
| 16     | 2    | texCoordsCentroid      | (`0` for everything we've seen)          |
| 18     | 1    | registerCount          | R-register count, sce-cgc minimum is 2   |
| 19     | 1    | outputFromH0           | 1 iff R0 is fp16 (= H0 in disasm)        |
| 20     | 1    | depthReplace           | 1 iff DEPTH output written               |
| 21     | 1    | pixelKill              | 1 iff KIL opcode emitted                 |

### Strings — per-parameter, NOT deduplicated

Order: walk parameters in declaration order; for each, emit the
semantic string (if non-empty) followed by the name string.  Both
NUL-terminated.  Region padded with NULs to a 16-byte boundary.

`identity_f.fpo` (two `: COLOR` params) emits "COLOR" twice — sce-cgc
does NOT collapse duplicates.  Verified:

```
0x80: COLOR\0color_in\0COLOR\0color_out\0\0
```

### Strings preserve source spelling

sce-cgc keeps the user's exact spelling for the semantic string:
- Source `: TEXCOORD0` → string "TEXCOORD0"
- Source `: TEXCOORD`  → string "TEXCOORD"

The resource code (`res` field) is the same in both cases
(CG_TEXCOORD0 = 3220 / 0x0c94), only the string differs.  Our front
end's stripped `semanticName` collapses both to "TEXCOORD"; we now
also store the raw source spelling in `Semantic::rawName` →
`IRParameter::rawSemanticName` and use it in the container emitter.

### Resource code mapping (FP profile)

| Cg semantic                | CGresource enum  | numeric |
|:---------------------------|:-----------------|--------:|
| `: COLOR` / `: COLOR0`     | CG_COLOR0        | 2757 (0x0ac5) |
| `: COLOR1`                 | CG_COLOR1        | 2758 (0x0ac6) |
| `: TEXCOORD` *n*           | CG_TEXCOORD0+n   | 3220+n (0x0c94+n) |
| sampler bound to TEXUNIT n | CG_TEXUNIT0+n    | 2048+n (0x0800+n) |

(VP profile uses different codes — `: COLOR` there is CG_COL0 =
2245 / 0x08c5.  The two profiles have disjoint enum subsets.)

### CGtype values (subset, from cg_datatypes.h, base = 1024)

| CG type        | numeric |
|:---------------|--------:|
| CG_FLOAT       | 1045 (0x0415) |
| CG_FLOAT2      | 1046 (0x0416) |
| CG_FLOAT3      | 1047 (0x0417) |
| CG_FLOAT4      | 1048 (0x0418) |
| CG_FLOAT4x4    | 1064 (0x0428) |
| CG_SAMPLER2D   | 1066 (0x042a) |
| CG_SAMPLERCUBE | 1069 (0x042d) |

### CGenum values (subset, from cg_enums.h)

| name        | numeric |
|:------------|--------:|
| CG_IN       | 4097 (0x1001) |
| CG_OUT      | 4098 (0x1002) |
| CG_INOUT    | 4099 (0x1003) |
| CG_VARYING  | 4101 (0x1005) |
| CG_UNIFORM  | 4102 (0x1006) |

## Open questions

- What's in c[0..255]? (reserved but for what — driver?  card state?)
- Full `attributeOutputMask` bit encoding — need more shader variants.
- Fragment profile (`sce_fp_rsx`) const allocation — not yet investigated
  (FP consts are inserted as inline 16-byte blocks AFTER the instruction
  that uses them, per the renouveau notes in nvfx_shader.h's header
  comment, but sce-cgc's specific layout and `embeddedConst` field
  haven't been mapped against real shaders yet).
- `#pragma alphakill <samplerName>`: **container-level only**, implemented
  2026-04-18.  Probed `tex2D` shader with and without `#pragma alphakill
  tex` — ucode bytes are bit-identical
  (`9e011700 c8011c9d c8000001 c8003fe1` in both).  The pragma adds a
  single extra `CgBinaryParameter` entry to the `.fpo`:
    - `type = 0x0418` (CG_FLOAT4)
    - `res  = 0x0cb8` (kill marker — new resource code, not in the
      previously catalogued enum)
    - `var  = 0x1005` (CG_VARYING)
    - `name = "$kill_<NNNN>"` (synthetic; `NNNN` zero-padded to 4 digits,
      indexed in source-order of the pragma occurrences, NOT by the
      sampler's position in the parameter list)
    - `direction    = 0x1002` (CG_OUT)
    - `paramno      = 0xFFFFFFFF` (synthetic — no user param number)
    - `resIndex     = 0xFFFFFFFF`
    - `isReferenced = 0` (the user code doesn't refer to it directly)
    - no semantic string
  The runtime presumably scans for parameters with `res=0x0cb8` and
  configures the GCM RSX state to discard fragments where the
  corresponding sampler returns alpha == 0.

  Wired into `rsx-cg-compiler` via:
  - `Preprocessor` collects samplers from `#pragma alphakill <name>`
    lines and exposes them via `alphakillSamplers()` (insertion-order
    preserved, deduped on name).
  - `sony::ContainerOptions::alphakillSamplers` carries the list to the
    container emitter; one `$kill_NNNN` `CgBinaryParameter` per name is
    appended to the parameter table after the entry-point's params.
  - Test: `tests/shaders/alphakill_f.cg` (320-byte .fpo, byte-exact
    against sce-cgc).

  Previous-session hypothesis about a "byte-1 bit 0x80" toggle in
  TEXR encoding was wrong — superseded by this finding.
- Array uniforms (`uniform float4 arr[8]`) — do they occupy contiguous
  slots in one region or the other?
- Is "simple first / multi-insn last" the complete ordering rule, or
  is there a finer tie-break when multiple multi-insn outputs exist?
  (Haven't tested a shader with two matvecmuls yet.)

## How to add an entry to this log

1. Run `wine sce-cgc.exe` on a test shader that exercises the feature.
2. Run `wine sce-cgcdisasm.exe` on the output to read the allocation and ucode.
3. Hex-diff the `.vpo` against a known-simple reference to isolate the
   bits that changed.
4. Add the test shader to `tests/shaders/` once the behaviour is
   locked into `nv40_vp_emit.cpp` (or `nv40_fp_emit.cpp`) and the
   byte-exact harness passes.
5. Append to this doc with the rule + the evidence (short trace or
   hex snippet).  Keep it terse — future-you should be able to scan
   this and find the answer without rerunning sce-cgc.
