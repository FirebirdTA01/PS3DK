# Render-target clear and readback

This sample switches from a display surface to a 64 × 64 offscreen surface,
clears it, waits for a fresh RSX completion label, and checks eight pixels
through the PPU mapping. It exits nonzero if any checked pixel differs from
`0xff004488`.

When running in RPCS3, enable **Write Color Buffers**. This test reads GPU
render-target memory from the PPU; without writeback, RPCS3 can expose the
original CPU fill instead of the rendered pixels. The completion label must
also advance for every wait so an earlier completion cannot satisfy a later
one.

This is a render-target switch, clear, and readback check. It does not test
sampling the result as a texture or establish hardware behavior.
