# hello-ppu-gcm-label-reuse

This sample tests whether a completion label is written late enough to reuse the main-memory buffers a frame read.

**One trial.** A trial makes 64 full-target draws from an arena mapped with `cellGcmMapMainMemory`; the draws read
vertices, indices and a texture. The completion label under test (application label 101) follows the draws. As soon
as the PPU sees that label pass, it poisons the arena. The rendered target is then copied back to main memory and
checked.

**Modes.** Four modes run, 32 trials each:

| Mode | Label |
|---|---|
| backend | `cellGcmSetWriteBackEndLabel` after the last draw |
| texture | `cellGcmSetWriteTextureLabel` after the last draw |
| command | `cellGcmSetWriteCommandLabel` after the last draw (control) |
| early | `cellGcmSetWriteCommandLabel` before the first draw (control) |

**Verdict.**

- `GCM_LABEL_REUSE_OK` means the backend and texture modes were clean. `GCM_LABEL_REUSE_FAIL` means they were not.
- `GCM_LABEL_REUSE_DETECTOR sensitive` follows. It means the early control was caught, so the detector can see a
  label that is too early on this platform.
- On RPCS3 the command control is clean too, because the emulator does not model the pipeline hazard. An RPCS3
  pass therefore says nothing about hardware.

The results and their limits are in [docs/gcm-labels.md](../../../docs/gcm-labels.md#3-what-a-completion-label-guarantees-a2).
