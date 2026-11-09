# Fragment-program control rendering

Three matching panels, left to right: normal program installation, installation
plus explicit safe control, installation plus reserved Unsafe control. The RSX
executes the same texture-sampling CGprogram in all three. The original private
command-buffer checks remain in source/checks.c and run before initialization.
ControlTxp and both reserved arguments are zero; bit 15 semantics remain open.

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
`/dev_hdd0/tmp/gcm-fp-control-*`. Successful startup checks, all pixel checks,
image publication and all 600 submissions are required for PASS. Partial receipts
have no authority; receipt=complete is required. Console output prints panel
geometry and mismatches. Hardware acceptance requires the matching receipt
and image, separately from host tests and builds.
