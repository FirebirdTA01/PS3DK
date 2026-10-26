# PSGL RGBA byte readback

This row checks `glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE)` on the current
default color buffer and a color0 texture framebuffer. It checks pack
alignments 1, 2, 4 and 8, row padding and guards, consecutive different
clears, lower-left row order, and alpha 255 for XRGB rendering surfaces.
The reference-texture pattern has distinct top-to-bottom rows and a
non-255 X byte, so copying X as alpha or reversing rows incorrectly fails.
An asymmetric drawn pattern independently checks both framebuffer paths:
red below green, with a blue one-pixel marker at lower-left coordinate (2,1).
Each target starts black so its first red probe cannot pass on a prior clear.

The 64 checks run under the default RPCS3 asynchronous shader configuration.
As in `shader-readback`, draws have at most ten warm-up attempts separated by
200 ms: a cold shader cache can otherwise skip a draw while completing its
GPU label. Attempt counts are printed, and all final pixels must still match
exactly. This is shader warm-up, not a retry of failed test runs. Synchronous
shader compilation is also supported; the test does not change emulator
settings. The explicit constant row fixture avoids conflating readback with
the separately investigated GCC 16 `-O3` Altivec row-initializer miscompile.

Negative dimensions, unsupported format/type pairs, incomplete framebuffers
and invalid pixel-store settings must report errors. Failed read queries
leave the output untouched. Pixels outside the source framebuffer are
undefined by GL; this implementation clips the DMA and leaves those output
bytes untouched. Zero-sized reads do not write output.

Invalid format/type tokens report `GL_INVALID_ENUM`, including `GL_FLOAT`,
unpacked `GL_UNSIGNED_SHORT`, and the excluded luminance read formats.
Recognized but unsupported pairs report `GL_INVALID_OPERATION`. Enum
validation also applies to zero-sized reads. The distinction follows
[ES 2.0 sections 2.5 and 4.3.1, tables 3.2/3.4](https://registry.khronos.org/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf).

This increment supports linear XRGB surfaces without multisampling.
Swizzled, multisampled and other color storage are refused. Each nonempty
in-bounds read allocates scratch storage rounded to 1 MiB, maps it for an
RSX image transfer, waits for GPU completion, unmaps it, and converts rows
to RGBA. This is a synchronous correctness path, not a throughput benchmark.
If unmapping fails, the mapped allocation is retained rather than freed.

The public contract follows OpenGL ES 2.0 section 4.3.1. It does not establish
complete ES 2.0 conformance, depth/stencil readback, hardware acceptance, or
correctness of unrelated framebuffer/scissor operations.
