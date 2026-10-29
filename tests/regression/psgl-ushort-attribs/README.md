# Unsigned-short GL attribute conversion

Each ABI runs 201 checks. The shader compares all four input components
against a separate current-value reference before producing framebuffer
values. A tolerance below 1e-7 distinguishes normalized 32767 from 32768;
literal references also cover 0, 1, 16384 and 65535. Failed components output
zero, so saturation cannot disguise signed or incorrectly normalized input.

The 144 matrix cases cover sizes 1-4, both normalization settings, client/VBO,
packed/padded/overlapping stride, and DrawArrays/u16/u32 EBO draws. Source
prefixes and padded strides are odd, requiring safe byte decoding. DrawArrays
uses first=2; indexed non-overlapping ranges have distinct values. Missing
components are compared against `(0,0,0,1)`; size4 checks provided alpha too.

Controls retain correct unnormalized size4 lower-half results and normalized
zero results. Normalized nonzero lower-half inputs are compared with a float
reference and are expected RED on the old signed, unnormalized descriptor.
Additional rows cover per-draw VBO updates, writable map/unmap updates, mapped
or insufficient input, overflow, indexed recovery and disabled current values.
The old path's unsafe negative draws are gated by a high-value conversion
probe; each gated case is recorded as a failure, not skipped from the total.

Build with the PPU CMake toolchain; private PSGL archive overrides are supported.
Baseline/fixed comparisons must use the same fixture and heap implementation.
RPCS3 results establish emulator behavior, not hardware conformance. Direct-Cg
and legacy conversion remain outside this routed GL repair.
