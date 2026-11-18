# Runtime chunk acceptance

The drainer updates its accepted ledgers only after the sink accepts the whole
framed chunk. This applies to the existing append sink and prepares the consumer
for bounded rolling admission. It does not enable rolling captures.

`enc_end` and `list_close` return acceptance. A short write followed by an error
still permanently fails an append sink; already written physical bytes are not
rolled back. No declaration or record in that rejected chunk becomes accepted.

The ledgers advance at these boundaries:

- THREADS: declaration generation and the generation's initial written count,
  followed by extension/wait notifications for exactly that chunk's identities.
- DESCRIPTORS: one past the last declaration in the accepted chunk.
- WINDOW: the accepted queue record's read cursor.
- RECORDS: the ring read cursor and non-GAP written count, independently for
  each chunk when a drain spans several chunks.
- RETIRED: the accepted generation and limit flag. Slot reuse follows accepted
  retirement. ARC builds additionally require successful arc transfer as before.

Frozen retirement follows the same limit-flag rule. Earlier accepted chunks
remain accepted if a later chunk fails. Resource refusal, physical failure,
quiescence, close, join and retention are distinct lifecycle decisions; this
change supplies the chunk acknowledgement boundary only.

The host controls use the actual APP-mode runtime and a callback that rejects
one selected chunk. They check THREADS, DESCRIPTORS, WINDOW_BEGIN, RECORDS and
RETIRED independently, plus second-chunk failures for large descriptor and
record populations. A successful control accepts every family and retires its
slot. All seven refusal cases must fail their named acknowledgement assertions
against the preceding runtime implementation; a compile failure is not evidence.

```sh
python3 tests/host/ps3dk-prof-rolling/run_runtime_ack.py --build-dir /path/to/ack
python3 tests/host/ps3dk-prof-rolling/run_runtime_ack.py \
  --cc 'cc -fsanitize=address,undefined -fno-sanitize-recover=all -ftls-model=local-exec -fno-pie -no-pie' \
  --build-dir /path/to/ack-sanitized
```

The explicit `--drain-source` argument supports an isolated prior source or
mutation without changing the checkout. Tests do not claim console filesystem
durability or completed rolling-runtime integration.
