# PSGL scalar cube texture subset

Scalar fragment `samplerCUBE` selections use logical texture units 0–7 through
`glUniform1i`, scalar `glUniform1iv`, and `glGetUniformiv`. A successful relink
resets selections to zero. Units have independent 2D and cube bindings. Named
textures acquire an immutable target at first bind; cube name zero has its own
context-owned face storage.

The supported storage is **level zero, uncompressed color converted to linear
ARGB8**, with a row pitch rounded to 128 bytes and six consecutive pitched face
slabs in +X, -X, +Y, -Y, +Z, -Z order. Each face must be square and at most 4096
texels wide. Byte color formats and the existing packed color inputs are
converted using the current unpack alignment. CPU copies retain the six faces;
draw preparation never reads RSX memory to reconstruct an image.

Mip levels above zero, compressed/depth images, swizzled allocation, framebuffer
attachments, copies and external texture-reference storage are refused for
cubes. This is a limited implementation, not a full OpenGL ES 2.0 claim.
Cube arrays and vertex samplers remain outside the supported GL subset; cube
array reflection refuses link. The compiler still refuses cube arrays.

A complete cube has six positive, equally sized faces with matching source
format/type and logical internal format. New cube objects use mipmapped min
filtering, linear mag filtering and repeat wrapping. With only level zero,
mipmapped filters make sizes above 1×1 incomplete. NPOT cubes require a nonmip
min filter and clamp-to-edge S/T wrapping. Incomplete cubes sample an immutable
opaque-black cube. Existing 2D defaults are unchanged.

Uploads validate before mutation. Allocation failures retain previous CPU face
data. Draw preparation uses a fresh packed allocation and waits for queued GPU
work before releasing the old allocation. Allocation, address-translation or
completion failure raises `GL_OUT_OF_MEMORY`, retains old storage and pending
CPU data, and skips the draw. Deletion also waits before changing ownership or
bindings. The black fallback is released after the shutdown completion wait.

DrawArrays and DrawElements reject active 2D and cube samplers selecting the
same logical unit with `GL_INVALID_OPERATION`, before emitting draw state or
staging vertices. This includes active array elements; unused reflected holes
do not conflict. Same-type sharing is allowed. Setters can create a temporary
conflict; the next draw validates it even when no other state is dirty.

Direct Cg binding retains physical texture slots and chooses the reflected
sampler target. `cgGLSetTextureParameter` and `cgGLSetupSampler` choose the cube
target for scalar cube parameters. This does not expand the other Cg texture
query helpers or fixed-function texture mapping.

The regression fixture exercises both PPU ABIs in RPCS3. **PS3 hardware
acceptance remains open** on dependent sample t_d7664220; neither emulator
results nor private layout measurements establish hardware support.
