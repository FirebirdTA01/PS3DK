# Small unnormalized unsigned-byte attributes

Both ABI rows use public GL shader linking with logical locations 3 and 6.
The vertex shader divides RGB by 255 and requires alpha to equal exactly 1,
emitting alpha zero otherwise. This avoids framebuffer clamping hiding an
incorrect default greater than 1. Sizes 1-3
must arrive as their integer values with missing components `(0,0,0,1)`.
Input `{64,128,255}` must produce `(64,0,0,255)`, `(64,128,0,255)`, or
`(64,128,255,255)`. This tests defaults and conversion through the shader.

The 54 basic combinations cover three sizes, client/VBO sources, packed,
padded and overlapping strides, and DrawArrays/16-bit/32-bit DrawElements.
DrawArrays uses a nonzero first vertex. Indexed draws use nonzero EBO
offsets and vertices 8-11; source pointers/VBO offsets also have a prefix.
Those indexed vertices have different red values from the DrawArrays range,
so an incorrect staged base cannot pass by selecting an equivalent color.
Additional checks cover VBO updates without another attribute setter,
short/mapped buffers, missing client pointers, span overflow, unsupported
client indices, index extent overflow, recovery and disabled current values.
Each executable has 72 checks, with bounded shader warm-up.

On a baseline that rejects these sizes, each affected check fails and skips
the draw. It never submits a rejected descriptor to the GPU. Use the PPU
CMake toolchain; optional `PSGL_ARCHIVE_ilp32` and `PSGL_ARCHIVE_lp64` paths
select private libraries. RPCS3 results are not hardware acceptance or
general GL conformance results. The conversion does not rely on whether
smaller UB256 descriptors are accepted by hardware.
