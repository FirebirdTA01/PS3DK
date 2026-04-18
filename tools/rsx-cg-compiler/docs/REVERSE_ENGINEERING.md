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

Observed in `two_vec4_v.cg` disassembly:

```
MOV o[0], v[0];
MOV R0, c[466];        // preload col_b into temp
ADD o[1], c[467], R0;  // col_a + R0 — one const, one temp
END
```

Our `rsx-cg-compiler` IR-to-NV40 lowering must honour the same
constraint — any arithmetic op with two uniform operands has to
insert a MOV-to-temp first.

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

## Open questions

- What's in c[0..255]? (reserved but for what — driver?  card state?)
- Full `attributeOutputMask` bit encoding — need more shader variants.
- Fragment profile (`sce_fp_rsx`) const allocation — not yet investigated.
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
