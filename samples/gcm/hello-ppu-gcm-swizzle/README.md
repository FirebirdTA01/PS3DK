# Swizzled texture rendering

Three matching 32x32 panels, left to right: linear reference, SDK conversion with
the default copy, SDK conversion with the copyTexel callback. The asymmetric
gradient, checker accents and diagonal expose orientation and stride errors.
Converted textures use the swizzled flag and zero pitch; the reference is linear
with 128-byte pitch. Each conversion has a 16 KiB allocation and remains in one
tile. The original 8x4 padding/guard/callback checks remain in source/checks.c.
This exercises dstLog2cdepth=2, four-byte texels and 2-D only; border, 3-D and
general parameter semantics remain open.

Build with the SDK environment set:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$PS3DK/cmake/ps3-ppu-toolchain.cmake"
cmake --build build
```

For a source checkout, use its cmake/ps3-ppu-toolchain.cmake. Keep the sibling
hello-ppu-cellgcm-quad shaders and gcm/common helpers present. Run the signed
SELF: it submits 600 frames and samples every source texel center in the first frame against the
source pattern. RGB tolerance is 2/255.
START exits early and cannot support PASS.

A fresh back-end label follows the framebuffer transfer to main memory. The
sample saves a PPM and a checked, published text receipt under
`/dev_hdd0/tmp/gcm-swizzle-*`. Successful startup checks, all pixel checks,
image publication and all 600 submissions are required for PASS. Partial receipts
have no authority; receipt=complete is required. Console output prints panel
geometry and mismatches. Hardware acceptance requires the matching receipt
and image, separately from host tests and builds.
