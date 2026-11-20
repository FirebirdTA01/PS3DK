# Retained rolling-capture listing

`ps3dk-prof rolling [--json] [--max-bytes N] capture.profr` reads a saved,
coherent snapshot. It validates the physical committed chain and every semantic
checkpoint transition before printing any result. A semantic failure in the
selected chain does not permit falling back to an older physical root.

This command is separate from the ordinary `check`, `report` and `auto`
commands. A retained report cannot establish a whole-run verdict from missing
events. The prefix checkpoint is an unauthenticated claim about overwritten
history; checksums establish integrity, not authenticity.

The JSON schema is 1. Both output formats use the same projection:

* `physical`: container version, OPEN/SEALED/STOPPED_RESOURCE state (or
  UNSPECIFIED for version 1), stop reason, root generation and recovery status.
* `retained` and `evicted`: original chunk frontiers and payload byte counts.
  Chunk numbers are never renumbered. Eviction is not producer GAP or LOSS.
* `source`: source trailer presence, separate from physical sealing. Mixed
  source completeness is unavailable; individual family evidence is reported
  below. A physically SEALED file can lack a source trailer.
* `ppu`: retained zone counts, open/left-censored counts and qualified raw PPU
  ticks. Tick sums are decimal strings, or null when unavailable.
* `spu`: retained events and pairs, loss and censoring facts. Placement is
  causal support only; counter history is not evaluated. There are no invented
  SPU durations from decreasing counters.
* `auto`, when present: the retained reader's clock-map result, frame verdicts,
  reasons and raw/qualified busy bounds, including unavailable boundary evidence.
* `fifo`, when present: retained event/accounting counts, missing records and
  prefix boundary frames. Prefix events are not retained measurements.

`capture_totals` is unavailable: this command does not reconstruct those totals
by adding retained rows or treating checkpoint claims as observations. Physical
sealing alone proves neither source completeness nor successful close/join.

Exit status is 0 for a valid retained listing, 1 for unsupported input or an
admission limit, 2 for malformed physical or semantic input, and 3 for arguments
or file IO. Status 0 is not a completeness or performance verdict. OPEN and
STOPPED_RESOURCE files may have valid retained listings.

The input cap defaults to 64 MiB. `--max-bytes` accepts a positive decimal byte
count that fits the host. The command checks the regular file's length before
allocation, reads exactly that admitted length, and checks for growth or
truncation. It does not provide a live snapshot retry protocol; the caller must
save a coherent snapshot first. Semantic state and work retain the reader's
independent bounded defaults, even when the input cap is raised.

Supported semantic checkpoints are PFRCKP01 (PPU/windows), and PFRMIX01
schema 1 (PPU/SPU/windows) or schema 2 (with AUTO/FIFO). The current rolling AUTO
decoder stops at CONFIG 1023/11; report-ring 2047/11 is not silently downgraded.
Manual GPU, auxiliary record families and exported streams remain subject to
explicit reader refusal. This command does not enable a public SDK rolling
capture option or change ordinary append capture defaults.
