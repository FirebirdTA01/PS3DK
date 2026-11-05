# CLKW1 marker matrix (W / W0)

The pinned stage/result/field matrix of the W probe's markers (design r4 + review 19:47). `host-tests/validate_w_markers.py`
holds the same table as data; anything not listed is FORBIDDEN. H8 = 8 lowercase hex digits; D = decimal.

## Every record

`CLKW1 run=<16hex> variant=<W|W0> build=<16hex> seq=<NN> stage=<name> result=<R> tb=<16hex> prev=<P> unres=<s>,<r> counts=<D>,<D>[ fields] len=<D>`

- `tb`: the timebase when the record was formatted (before its own open). Readings may be EQUAL: every time order is
  nondecreasing and no positive duration is inferred.
- `prev`: `none` for 01 only; otherwise `<NN>:<IO>:<16hex>` naming the previous ATTEMPTED marker, its FIRST I/O error
  and the timebase read after its LAST attempted call. `IO` = `ok | error:<open|write|fsync|close>:<H8> | error:write:zero |
  error:format:<record|path>` (no errno is manufactured for the last two). Every normal record's prev is `ok`; only 99
  names a failed predecessor.
- `unres`: SPU `n | u | q`, RSX `n | u`; sticky. `u` = submitted, no trusted completion (a poll timeout or an identity
  mismatch); `q` = a mailbox-write error whose submission outcome is unknown. Historical only; never "still active".
- No record attests its own I/O; the last record's own outcome is UNOBSERVED.

## Stages

| seq | stage | results | fields |
|---|---|---|---|
| 01 | main-entered | ok | - |
| 02 | csv-opened | ok, error:format:path (the CSV name did not fit: no open is issued), error:open:H8, error:fdopen:H8 (c4's open(O_EXCL) then fdopen) | rawclose=<ok\|H8> present iff error:fdopen (the raw fd's close result) |
| 03 05 07 09 13 15 | *-begin | ok (written only when the stage starts) | - |
| 11 | write-req-begin | ok, W only, written BEFORE the mailbox write | - |
| 04 | spu-setup-done | ok, error:<sysutil-register\|init\|import\|group-create\|thread-init\|group-start>:H8, skipped:csv-failed | - |
| 06 | gcm-setup-done | ok, unavailable:<host-buffer\|gcm-init>:H8, skipped:<csv-failed\|spu-setup-failed> | - |
| 08 | phase0-done | ok, timeout, identity, mbox-refused:H8, mbox-error:H8, quit, csv-stopped, skipped:<csv-failed\|spu-setup-failed> | issued=D [last=H8] csv=C |
| 10 | rsx-done | ok, timeout, identity, quit, csv-stopped, unavailable:label-address, skipped:<csv-failed\|spu-setup-failed\|gcm-unavailable\|quit> | issued=D [last=H8] csv=C |
| 12 (W) | write-req-done | ok, timeout, identity, mbox-refused:H8, mbox-error:H8, skipped:<csv-failed\|spu-setup-failed\|spu-stopped\|quit> | [dec=H8 dec2=H8 wrote=H8, only with ok] csv=C |
| 12 (W0) | write-req-done | skipped:<csv-failed\|spu-setup-failed\|variant-w0> | csv=not-attempted |
| 14 | phase1-done | as 08, plus skipped:<spu-stopped\|quit> | as 08 |
| 16 | teardown-done | ok, error:<terminate\|join\|destroy\|image-close>:H8, skipped:csv-failed | left_running=<0\|1> spu_join=<ok\|failed\|not-attempted\|not-started> |
| 17 | csv-closed | ok, error, skipped:csv-failed | fflush=X fsync=X fclose=X (X = ok \| H8 \| not-attempted) |
| 99 | logging-failed | failed (prev names the failed marker and its first error) | - |

C = `ok | row-write | fflush:H8 | fsync:H8 | not-attempted`.

## Rules

- Widths and bounds: counts <= 2^32-1 and <= the build's bounds (SPU 2 x CAL_SPU_SAMPLES, RSX CAL_RSX_SAMPLES); issued
  <= the phase target; `last` is exact: 08 and 10 `last == issued`; 14 `last == CAL_SPU_SAMPLES + issued (+1 in W, the
  write request's number)`; 08's SPU count == issued (minus the failing one after a timeout/identity).
- Predecessors (normal records): 02<-01, 03<-02, 04<-03|02, 05<-04, 06<-05|04, 07<-06, 08<-07|06, 09<-08, 10<-09|08,
  11<-10, 12<-11|10, 13<-12, 14<-13|12, 15<-14, 16<-15|14, 17<-16 (W0 never names 11); 99 names any attempted milestone
  of its variant. Across present records, no present marker may lie between a record and the predecessor it names.
- unres vs the record's own result: 08/12/14 timeout or identity -> SPU `u`; mbox-error -> SPU `q`; mbox-refused and every
  other non-skipped result at 08/12/14 -> SPU `n`; 10 timeout/identity -> RSX `u`, other non-skipped 10 results -> `n`;
  before 08 both `n`, before 10 RSX `n`.
- Transitions over present records: replaying them through the skip precedence below, every later present record
  must be what the producer writes in that state (e.g. after a present SPU stop, a present 13 or a non-spu-stopped 12
  is a contradiction even if the skipped marker between them is missing). An inconsistent run prints only REACHED
  and PROCESS_EXIT=UNOBSERVED (derived facts WITHHELD).

- Phase fields: `issued` = requests SUBMITTED in that phase (the mailbox write, or the GCM flush, returned); `last` is
  present iff issued >= 1. Every SKIPPED phase-done record has `issued=0`, no `last`, `csv=not-attempted`. `ok` means
  issued == the target (CAL_SPU_SAMPLES / CAL_RSX_SAMPLES); `quit` and `csv-stopped` mean issued < the target;
  timeout/identity follow a submitted request (issued >= 1); `unavailable:label-address` issues nothing; `csv-stopped`
  (a CSV row write failure detected before the next request) never attempts the checkpoint.
- 08 is never `skipped:quit`: no exit request can be seen before phase 0 (c4 only polls sysutil inside its loops).
- Skip precedence when several conditions hold: the experiment-stopping cause (csv-failed, spu-setup-failed) first; then
  W0's omission of 12 (variant-w0); then gcm-unavailable (10); then spu-stopped (12, 14); then quit.
- Mailbox errors: `mbox-error:H8` = the submission outcome is UNKNOWN (unres q); `mbox-refused:H8` only for a code the
  producer can classify as a guaranteed no-enqueue refusal. On target NO code is so classified (no LV2 contract is
  known); the host model has one (0x80010099) to test the path.
- CSV checkpoints (after 08/10/12/14): fflush, then fsync only if fflush succeeded; the first error is kept and stops the
  experiment (the remaining measurement stages are skipped:csv-failed; teardown runs and is still marked). Final close
  (17): after an earlier CSV failure fflush/fsync are not attempted; fclose is attempted exactly once whenever a FILE
  exists. A CSV row write failure is detected before the next request and at each checkpoint (csv=row-write).
- Logging failure: no further normal marker, no further measurement request; ONE 99 is attempted, never retried;
  teardown and the CSV close still run (their results go to the CSV only); the process exits nonzero.
- The CSV analyzer's verdict is independent of the sidecar: failures that happen after the CSV's end row (the final
  fflush/fsync/fclose, a marker failure at 16 or 17) can leave a CLEAN analyzer verdict; the marker and exit facts
  record them. Not every I/O failure implies a non-clean analyzer verdict.
- 16: skipped => left_running=0 spu_join=not-started; error:terminate => 1 / not-attempted; error:join => 1 / failed;
  ok, error:destroy, error:image-close => 0 / ok or not-started. spu_join=ok (SPU_PRODUCER_QUIESCED) never clears unres.
- 17: ok needs all three ok; fsync is not-attempted when fflush failed; error may carry all three ok (a CSV row write
  failed after the last checkpoint).
- Cross-record (validator, per run; absent records are UNKNOWN): prev seq < seq; the named predecessor's tb <= its
  completion time <= this tb; unres sticky; counts nondecreasing; no normal record after the marker a valid 99 names;
  no SPU request stage runs after skipped:spu-stopped; one variant per run.
