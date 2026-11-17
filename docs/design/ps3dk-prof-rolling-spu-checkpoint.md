# Standalone SPU restart state

`ext::rolling::Adapter` is a bounded, family-only restart adapter. It does not
enable rolling capture in the SDK, join SPU state to the physical rolling
container, or qualify SPU clock ticks. Ordinary version-1 parsing is unchanged.

The caller validates original framing, PPU declarations and PROFILE at their
original positions. Each `accept` call supplies the actual original chunk index
and the PPU references available there. Import requires trusted references for
the recorded history: a final dictionary alone does not establish that a
declaration preceded its use. A combined importer must enforce that chronology.

The supported capability masks are 1, 3 and 7, optionally with WINDOWS, and the
only foreign domain kind is SPU_DEC. Unsupported families are refused. Any
rejected input poisons the adapter, preventing subsequent checkpoint publication.

## State and validation

The version-1 `PFRSPU01` checkpoint has a 64-byte little-endian header, a typed
journal and a trailing CRC32. Support entries retain their original chunk index,
type and payload. Event blocks retain their first and last original chunk index
and independent per-lane runs. A run records its epoch, first sequence, event and
chunk counts, terminal pairing state and active BEGIN witnesses. It does not
retain completed prefix event rows.

Cross-lane event interleaving between support records is intentionally omitted:
the lane streams are independent in that interval. The sum of run chunk counts
must fit the block's distinct original positions; a one-chunk block must have
equal endpoints. No per-lane original-position claim is encoded. Adjacent event
blocks coalesce, giving one canonical representation across such interleavings.

Support records are replayed through the existing extension parser at the
correct source cursors. Typed event runs advance those cursors without fabricating
raw events. The importer checks sequence tiling, epoch identity, stack witnesses,
sticky protocol errors, bounded transitions, byte counts and final equations.
DOMAIN_FINAL and ORDER_FINAL retain their existing validation. CRC and structural
consistency do not authenticate an evicted prefix; the first checkpoint is a
bounded producer claim about that history.

## Retained results and limits

Results separate prefix and retained event/loss counts. A retained END whose
BEGIN is checkpoint support is left-censored; complete interior pairs remain
available. Historical loss and pairing invalidity survive import. Open prefix
and retained BEGINs are distinguished, and a missing source final remains
incomplete. Calibration, epoch and ORDER support is retained without asserting
that it proves tick lifting over observations that were evicted.

Default limits are 8 MiB encoded checkpoint, 4 MiB support journal, 1,024
sources, 65,536 event runs, 65,536 stack witnesses and one million raw events
processed between imports. These are independent limits, not a claim that all
maxima fit simultaneously. Import is bounded by the encoded checkpoint and
typed state; it does not expand evicted event counts. Exhaustion is an explicit
error. Runtime preallocation and accepted-output state are separate integration
work.

The host tests compare source support and cumulative counts at every chunk
boundary, exercise crossing pairs, loss and epoch transitions, and repair CRCs
after mutations to test semantic refusals. This adapter alone is not a complete
rolling SPU measurement surface: the combined-family envelope, counter-history
qualification and runtime integration still need their own gates.

`finish_with_causal` additionally computes retained endpoint lower bounds and
drain/calibration upper bounds from the original ORDER support. It checks
historical causal contradictions over compact event groups without expanding
evicted event counts. Loss, missing finals, dropped ORDER evidence, invalid
support and missing bounds remain explicit refusals. Exhausting its independent
work or storage budget returns no partially qualified endpoint table.

This result is labeled `CausalSupportOnly` with counter history `NotEvaluated`.
It does not evaluate evicted raw counters, modular tick lifting or RMAX1, and
does not claim SPU durations. Its default budgets are 16,777,216 work units,
262,144 groups, 65,536 retained endpoints and 32 displayed contradiction
witnesses; the display limit never disables contradiction checking. Host tests
compare causal endpoint bounds against the ordinary reader of the full original
capture and exercise historical contradictions and resource refusals.
