# DXT textures on the RSX

Three rows compare uncompressed reference (left) and compressed sampling (right):
DXT1, DXT3, DXT5. Each 8x8 image has red, green, blue and white blocks. DXT3/5
contain an alpha ramp blended over a dark background; DXT1 ignores source alpha
in both panels. source/assets.h embeds the real host dxt-example output with
SHA256 identities. Encoding stays on the host; console uploads preserve bytes.
The DXT5 panel permits 20/255 RGB error for this fixture's alpha quantization
and blending. This is not a general encoder quality guarantee.

Regenerate from this sample directory with the native host compiler selected:

```sh
cmake -S ../../../tools/dxt-encoder -B host-dxt
cmake --build host-dxt
host-dxt/dxt-example input.rgba tiles.dxt1 tiles.dxt3 tiles.dxt5
python3 generate-assets.py input.rgba tiles.dxt1 tiles.dxt3 tiles.dxt5 source/assets.h
```

Build with the SDK environment set:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$PS3DK/cmake/ps3-ppu-toolchain.cmake"
cmake --build build
```

For a source checkout, use its cmake/ps3-ppu-toolchain.cmake. Keep the sibling
hello-ppu-cellgcm-quad shaders and gcm/common helpers present. Run the signed
SELF: it submits 600 frames and samples every source texel center in the first frame against the
source pattern. RGB tolerance is 2/255 except the DXT5 panel described above.
START exits early and cannot support PASS.

A fresh back-end label follows the framebuffer transfer to main memory. The
sample saves a PPM and a checked, published text receipt under
`/dev_hdd0/tmp/gcm-dxt-*`. Successful startup checks, all pixel checks,
image publication and all 600 submissions are required for PASS. Partial receipts
have no authority; receipt=complete is required. Console output prints panel
geometry and mismatches. Hardware acceptance requires the matching receipt
and image, separately from host tests and builds.
