# DXT asset example

This portable C encoder runs on the build host. It is not an installed PPU
SDK library. Build the example from a source checkout with a native compiler:

```sh
cmake -S tools/dxt-encoder -B build/dxt
cmake --build build/dxt
ctest --test-dir build/dxt --output-on-failure
build/dxt/dxt-example input.rgba tiles.dxt1 tiles.dxt3 tiles.dxt5
```

The example makes an 8x8 RGBA8 image with red, green, blue, and white 4x4
blocks in row order. Each block has alpha 0,17,...,255 in row order. It checks
block order, color endpoint bytes, explicit alpha bytes, and decoded DXT5
alpha error before writing the four files. With no arguments it only checks.
An error returns nonzero; output files from a failed write must be discarded.

Input bytes are R,G,B,A, independent of host endianness. Compressed blocks
are little-endian, in block-row order. DXT1 takes 8 bytes per block;
DXT3 and DXT5 take 16. The files are raw payloads: no container, dimensions,
mipmaps, or GPU upload is included. The example has not validated RSX texture
sampling. Use the payload with the matching texture format, not an
uncompressed RGBA format.

`dxt1_encode_*` currently encodes RGB only and ignores source alpha; use
DXT3/5 when alpha matters. Bounding-box color selection is deliberately
simple and can have large errors for unrelated colors within one block.
The example's solid primary colors are lossless, not a general quality claim.

DXT5 selector scoring and the host test decoder use truncating integer alpha
interpolation. The example's error bound applies to that convention. For
endpoints 255 and 0, rounding to nearest instead changes entries 2, 4 and 6
from 218, 145 and 72 to 219, 146 and 73. The stored endpoints and selectors
remain valid; GPU interpolation rounding has not been measured. Tests also
pin all eight selector codes in literal packed bytes independently of decoding.

For non-multiple-of-four dimensions the image encoder repeats edge texels
in the last block. Callers supply valid buffers sized for RGBA input and
`ceil(width/4) * ceil(height/4)` output blocks; these routines are intended
for bounded asset inputs and do not take output-capacity arguments.
