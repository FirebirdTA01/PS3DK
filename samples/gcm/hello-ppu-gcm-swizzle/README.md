# CPU swizzle conversion

Converts an 8x4 rectangle of four-byte texels into the first tile of a
32x32 destination using `cellGcmConvertSwizzleFormat`. Both the default copy
and a `copyTexel` callback are demonstrated. The sample checks every output
byte, untouched padding, guard bytes, and the callback count. A mismatch
returns 1; success prints two PASS lines and returns 0.

With the SDK environment set:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$PS3DK/cmake/ps3-ppu-toolchain.cmake"
cmake --build build
```

For a source checkout, use the repository's `cmake/ps3-ppu-toolchain.cmake`.
Run the resulting SELF and read its console output. There is no display loop.

The current converter uses a 16 KiB tile allocation and interprets
`dstLog2cdepth=2` as four bytes. This sample documents that implementation's
behavior. It does not establish the unresolved parameter semantics, border
handling, 3-D layout, or correct RSX sampling of the converted bytes.
