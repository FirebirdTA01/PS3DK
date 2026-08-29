# rsx-cg-compiler conformance — design

Board task: `t_3178b037` (design first; no implementation until the director names it the milestone).
Director's framing (2026-08-29): "improving our shader compiler and testing it against the same samples
listed as the tests for" ps3dev/PSL1GHT discussion #170. Recon (local-only, not in the repo):
`docs/local/shader-compiler-170-recon.md`.
Status: **draft v1, 2026-08-29.** Everything below is derived from our own tree and public sources; the
GPL toolkit is an oracle we *run*, never a source we copy.

## 0. Decisions

| Question | Decision | Why |
|---|---|---|
| What "the #170 test set" is | The PS3 OpenGraphics Toolkit's `testsuite/shaders/`: **86 fragment shaders + 1 vertex shader, plain Cg** (`test_NN_<feature>.fcg`, one intrinsic or hardware feature each), plus two real-world ports (IoQuake3-PS3, xash3d-fwgs). The discussion names no PSL1GHT samples. | Verified by reading the listing and one shader (standard Cg with `main` + semantics, cgc-compatible). |
| Corpus | **Our own MIT corpus with the same 86 feature names**, one Cg shader per feature, in `tools/rsx-cg-compiler/tests/conformance/`. The GPL `.fcg` files are never vendored; an optional local-only script may fetch them at a pinned commit into a gitignored dir as an *extra input set*. | Feature names are facts; the shader text is expression. Inputs to our tool do not attach GPL; copies in the repo would. |
| Pass criterion | **Three tiers, and tier 3 is the one that counts:** (1) compile + container check; (2) disassembly round-trip; (3) **render-to-RT readback on RPCS3** compared with PPU-computed expected values, `PASS/FAIL <name>` on TTY, non-zero exit on any failure, run by the regression battery. | The toolkit's harness renders each test to a quad with a banner and judges nothing — no readback, no expected values, no PASS/FAIL. "Test against the same set" has to mean something falsifiable. |
| Oracles | NVIDIA `cgc` output for the same Cg where available (`cgcomp` path; `cg.dll` is not shipped, check `C:\SDKs\Sony\SCE\PS3_450`), the toolkit's `rsxcomp`/`rsxdeasm` run as black boxes, RPCS3 as the runtime. | Same three-oracle method as blitting and PRX. |
| Scope order | Fragment-program features first (the 86 are fragment tests; the `v_` ones exercise vertex-program features through a fragment harness), vertex-program parity second, ports last. | Matches where the test set puts weight and where our back-end has gaps. |

## 1. Baseline (verified in-tree, 2026-08-29)

`tools/rsx-cg-compiler/` = vita-cg donor front-end (preprocessor → parser → AST → semantic → IR + IR
passes, `src/donor/`) + our NV40 back-end (`src/nv40/`: general lowering, if-conversion to
predication, FP/VP emit + assemblers) + container emit (`.fpo`/`.vpo`, PSL1GHT-runtime compat).
Own tests: **18 shaders** in `tests/shaders/` (arithmetic, mad chains, dot, min/max, swizzle, struct
texcoords, pow, refract, phong-style). Usage: **20 samples on `ps3_add_cg_shader_rsxcgc`**, 4 still on
`cgcomp`; 43 `.vcg/.fcg` in `samples/`.

**FP opcodes the back-end emits today** (from `nv40_fp_emit.cpp` / `nv40_fp_assembler.cpp`):
`ADD COS DP2 DP3 DP4 EX2 KIL LG2 MAD MAX MIN MOV MUL RCP RSQ SEQ SGE SGT SIN SLE SLT SNE TEX TXP`
(+ `DIVRSQ`, `SHIFT` helpers). Flow control is handled only by `nv40_if_convert` (if/else diamonds →
CC-predicated writes); there is **no** emit path for `DDX/DDY`, `IF/ELSE/ENDIF`, `REP/LOOP/BRK`,
`TXL/TXD/TXB`, `PK*/UP*`, `LIT`, `BEM`, `DEPR` (depth output), `FACE`, `NRM`, `DP2A`, `RFL`, or
multiple colour outputs.

**VP opcodes the back-end emits today** (`VP_OP(...)` sites in `nv40_vp_emit.cpp`, 4.2k lines):
`MOV ADD MUL MAD DP3 DP4 DPH MIN MAX FLR ARL`; `PSIZE` and `FOG`/`FOGC` outputs are mapped;
relative (`ARL`-based) constant addressing exists. Scalar ops (`RCP/RSQ/EXP/LOG/LIT/SIN/COS`) go
through a separate scalar-slot path not enumerated here (phase-1 item). No vertex-program branch,
call/return, loop, predicate, or vertex-texture (`TXL`) emit exists.

## 2. Feature map: the 86 vs the back-end

| # | Feature (toolkit name) | Cg construct | NV40 FP need | Status today |
|---|---|---|---|---|
| 02–11 | add sub mul mad div rcp rsq sqrt abs neg | arithmetic | ADD/MAD/RCP/RSQ + modifiers | **have** (sqrt = RSQ·RCP or x·RSQ; verify precision vs cgc) |
| 12–18 | sin cos tan asin acos atan atan2 | transcendental | SIN/COS native; the rest are library expansions | SIN/COS **have**; tan/asin/acos/atan/atan2 need the Cg stdlib expansions (front-end) — check donor stdlib |
| 19–26 | radians degrees exp2 exp log2 log log10 pow | EX2/LG2 + MUL | **have** EX2/LG2; pow = EX2(LG2) — verify |
| 27–32 | floor ceil frac round trunc sign | FLR/FRC/SSG | FRC yes? FLR/`SSG` **missing** (FLR = x−FRC(x); sign via SGT/SLT) — verify |
| 33–39 | min max clamp saturate lerp step smoothstep | MIN/MAX/sat modifier/MAD/SGE | **have** (saturate = result modifier) |
| 40–49 | dot2/3/4 cross length distance normalize reflect refract faceforward | DP2/DP3/DP4, RSQ, NRM | **have** except `NRM` (use DP3+RSQ+MUL) |
| 50–55 | slt sge sgt sle seq sne | compares | **have** |
| 56 | deriv (ddx/ddy) | `ddx()/ddy()` | `DDX`/`DDY` | **missing** |
| 57 | discard | `discard` / `clip()` | `KIL` | **have** (verify with CC predication) |
| 58–68 | v_input_attributes, v_output_registers, v_temporary_registers, v_constant_pool, v_address_register, v_texture_fetch, v_dual_issue, v_predicate_register, v_point_size, v_fog_factor, v_flow_control | vertex-program features | VP: ARL, VP-side TEX (NV40 vertex texture fetch), dual-issue scheduling, predicate writes, PSIZ/FOGC outputs, VP branch/call | inputs/outputs/temps/constants/ARL/PSIZE/FOGC **have**; vertex TEX, dual-issue scheduling, predicates, branch/call/loop **missing** |
| 69 | mrt_gbuffer | `COLOR0..COLOR3` outputs | multiple output regs (R0/R2/R3/R4 → MRT) | **missing** |
| 70 | dynamic_branching | `if` on a varying/uniform with side effects | FP `IF/ELSE/ENDIF` (fp40) | **missing** (predication only) |
| 71 | v_dynamic_loops | `for` with uniform count | FP `REP/LOOP/BRK` (fp40) / VP loops | **missing** |
| 72–73 | v_integer/boolean constants | int/bool uniforms | VP int/bool constant regs | **missing** (no int/bool constant path in the VP emitter) |
| 74 | shadow_map_pcf | `tex2D` on a depth texture + compare | shadow TEX (sampler state) + compares | partial |
| 75 | native_3d_geometry | 3D texture | `TEX` target 3D | `tex3D` **missing** |
| 76 | custom_depth_output | `DEPTH` semantic | `DEPR` (depth write to R1.z) | **missing** (grep hits are the semantic name only — verify) |
| 77 | texture_derivatives_lod | `tex2Dlod/tex2Dbias/tex2D(…,dx,dy)` | `TXL/TXB/TXD` | **missing** |
| 78 | volume_cube_sampling | `tex3D/texCUBE` | `TEX` targets | `texCUBE` partial, `tex3D` **missing** |
| 79 | centroid_interpolation | `centroid` qualifier | input attribute interpolation flag | **missing** |
| 80 | two_sided_vface | `FACE`/`VFACE` semantic | `FACE` input register | **missing** |
| 81 | vector_packing_unpacking | `pack_2half/unpack_2half` etc. | `PK2H/UP2H/PK4B/UP4B/PK2US/UP2US` | **missing** |
| 82 | hardware_lighting_lit_dst | `lit()` | `LIT` | **missing** |
| 83 | bump_env_mapping_bem | `BEM`-style texture offset | `BEM` | **missing** |
| 84 | projective_texture_txp | `tex2Dproj` | `TXP` | **have** |
| 85 | bitwise_halfword_ops | half-precision ops / packing | `half` type + `PK/UP` | **missing** |
| 86 | extended_address_stack | nested indexing | VP address-register stack (`PUSHA/POPA`) | **missing** (single `ARL` only) |

Tests 69–86 are unimplemented in the toolkit itself; we can be strictly better there, and the ones
that matter for real programs are 70/71 (branching, loops), 76 (depth), 77 (LOD/derivatives),
78 (3D/cube), 81 (packing), and 69 (MRT).

## 3. The conformance harness (the deliverable that makes "PASS" mean something)

`samples/toolchain/hello-ppu-shader-conformance` (one PPU program, one SPU-free GCM context):

1. For each corpus shader, the CMake side compiles `<name>.fcg` (+ a shared pass-through `.vcg`) with
   rsx-cg-compiler; `bin2s`/`ps3_add_cg_shader_rsxcgc` embeds the `.fpo`.
2. At runtime the program binds a small render target (e.g. 64×64 RGBA8 **and** a float16 target for
   precision tests), draws a full-screen quad with the shader, with inputs chosen so each pixel's
   expected value is computable on the PPU (texcoord-driven inputs, fixed uniforms, a 4×4 test
   texture for the texture tests).
3. Reads the RT back (`cellGcmSetTransfer*` to main memory + sync — the blitting path we already
   validated), compares N sample pixels against PPU-computed expectations with a per-test tolerance
   (ULP-style for float16, ±1 LSB for RGBA8), prints `PASS <name>` / `FAIL <name> px=(x,y)
   got=… want=…`, and exits non-zero if any failed.
4. `tests/regression/manifest.txt` gains a row; the battery's forbidden-TTY regex covers `FAIL `.
5. Compile-only tests (tier 1) live in `cargo`/CMake unit tests: every corpus shader must compile,
   and `abi-verify`-style container checks pass; tier 2 disassembles the emitted ucode with our own
   dumper and compares against a checked-in expected listing (regenerated deliberately).

Expected values come from a tiny C reference implementation of each intrinsic on the PPU
(`float` math), which is also where tolerances are documented. For 74/79/80 (PCF, centroid, VFACE)
the expectation is structural (which pixels are lit / which face) rather than numeric.

## 4. Oracle comparisons (run-only, evidence only)

* `cgc`/`cgcomp` on the same corpus where the Cg runtime exists (`C:\SDKs\Sony\SCE\PS3_450` PC tools
  slice, or the CachyOS reference host): compare disassembled ucode shape (instruction count, opcodes
  used, register pressure) — not bytes.
* The toolkit's `rsxcomp` + `rsxdeasm` as black boxes on the *original* 86 (local-only fetch): which
  of 69–86 it stubs vs which we implement.
* RPCS3 readback (tier 3) is the arbiter for correctness; the two above are for optimisation quality.

## 5. Phases and estimate

| Phase | Deliverable | Size |
|---|---|---|
| 1 | VP opcode inventory; corpus skeleton (86 + our 18 renamed into the scheme); tier-1 compile gate in CI; harness sample with readback + 10 arithmetic tests wired into the battery | ~1 session |
| 2 | Fill the "verify" rows (sqrt/pow/floor/sign/normalize precision vs cgc); implement `DDX/DDY`, `TXL/TXB/TXD`, `tex3D`, `DEPR`, `FACE`, `LIT`, `NRM`, `DP2A`; tests 02–57, 74–84 green | ~2 sessions |
| 3 | Real flow control: FP `IF/ELSE/ENDIF`, `REP/LOOP/BRK` emit (keep if-conversion as the fast path); MRT outputs; pack/unpack + `half`; tests 69–71, 81, 85 | ~2 sessions |
| 4 | Vertex-program parity (58–68, 72–73, 86): VP TEX, predicates, PSIZ/FOGC, loops, address stack | ~1–2 sessions |
| 5 | Migrate the 4 `cgcomp` samples to rsx-cg-compiler; run IoQuake3-PS3 and xash3d-fwgs shader sets through the compiler (compile tier), boot what we can on RPCS3 | ~1 session |

Non-goals for v1: GLSL/SPIR-V front-ends (the toolkit's input path), a Cg *runtime*, NV40 vertex
texture formats beyond what RSX exposes, performance parity with cgc `-O3`.

## 6. Open questions for the director / CachyOS

* Is `cgc` (Cg toolkit) present on the CachyOS reference host for oracle runs? (Windows has only the
  PS3_450 PC-tools slice; `cg.dll` is not shipped in our zip.)
* Tolerance policy for transcendental functions vs cgc's fast-math expansions — match cgc or match
  the mathematically exact value?
* Whether the conformance sample ships in the zip (it would add ~90 embedded shaders to `samples/`)
  or lives in `tests/regression/` only (recommend `tests/regression/`, like `librt-posix`).
