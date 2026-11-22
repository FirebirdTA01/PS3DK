# DXT textures on the RSX

Three rows compare the uncompressed source (left) with the compressed texture
(right): DXT1, DXT3, DXT5. Each 8x8 image has red, green, blue and white 4x4
blocks with an alpha ramp. The DXT3 and DXT5 rows blend over a dark
background; the DXT1 row draws without blending, so alpha is ignored in both
of its panels. Sampling is nearest-filtered and each panel is an integer
magnification of the 8x8 image.

`source/assets.h` embeds the output of the host example in
[`tools/dxt-encoder`](../../../tools/dxt-encoder/README.md), with a SHA256 per
payload. Encoding stays on the host; the console copies the bytes unchanged.
`tests/sdk/gcm-dxt-assets-test.sh` rebuilds the example, regenerates the header
and requires it to match the committed one, so an encoder change that alters
these bytes fails until the header is regenerated. To regenerate it from this
directory with a native host compiler:

```sh
cmake -S ../../../tools/dxt-encoder -B host-dxt
cmake --build host-dxt
host-dxt/dxt-example input.rgba tiles.dxt1 tiles.dxt3 tiles.dxt5
python3 generate-assets.py input.rgba tiles.dxt1 tiles.dxt3 tiles.dxt5 source/assets.h
```

## Build

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$PS3DK/cmake/ps3-ppu-toolchain.cmake"
cmake --build build
```

For a source checkout, use its `cmake/ps3-ppu-toolchain.cmake`. The shaders in
`shaders/` are the textured-quad pair from `hello-ppu-cellgcm-quad`.

## Run and check

The sample submits 600 frames (VSYNC, double buffered). After the draws of
frame 0 it fences, copies the framebuffer to main memory with
`cellGcmSetTransferImage`, fences again and checks the center of every source
texel in all six panels against the source pattern: 384 checks. The RGB
tolerance is 2/255, except the DXT5 panel, which allows 20/255 for this
fixture's 3-bit alpha interpolation after blending. That bound is specific to
this fixture, not a general encoder quality claim. The readback area is filled
with a canary first, so an incomplete copy shows up as mismatches.

The TTY prints the panel geometry, one line per panel with its mismatch count,
the drain result and a final line:

```
gcm-dxt: PASS frames=600 pixel_checks=384 pixel_failures=0 image=... receipt=...
```

The image (`/dev_hdd0/tmp/gcm-dxt-<run>.ppm`) and a receipt
(`/dev_hdd0/tmp/gcm-dxt-<run>.txt`, ending `receipt=complete`) are written
through a temporary file and renamed. PASS needs every pixel check, the saved
image, the receipt and all 600 frames; START or a quit request ends the run
early, and an early end is FAIL. The exit status is 0 only on PASS.

## GPU safety

The fence label (200, application range) is zeroed and read back before any
GPU work. Every wait (each flip, both readback fences) is bounded and services
system callbacks; the whole run is bounded at 60 s. A failure stops
submitting and goes to the exit drain, which waits for the last flip and a
final fence. If the drain cannot prove the RSX idle the sample prints
`gcm-dxt: RECOVERY_REQUIRED at=<where>` and parks without sending another
command or returning.

The fence after the transfer is a back-end label: on hardware, that it retires
only after the transfer engine has written the copy is not established. The
canary turns an early read into mismatches rather than a false PASS.
