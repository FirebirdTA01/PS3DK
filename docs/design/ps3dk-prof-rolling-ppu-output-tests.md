# Private PPU accepted-output integration evidence

This host fixture connects the real private `pfp_prepare` encoder, `pfo_*`
coordinator and `pfr_*` physical store. It exercises their existing contracts;
it does not enable rolling capture. There is no profiler-core wiring, producer
acknowledgement, lifecycle, live-reader or PS3 filesystem claim here. See the
[coordinator contract](ps3dk-prof-rolling-output.md) and
[selected PPU encoder profile](ps3dk-prof-rolling-ppu-encoder.md).

## What is independently checked

`ppu_output_fixture.c` receives only complete original `.prof` bytes and caller
capacities. The Rust integration test first validates that original stream.
No Rust checkpoint or serialized semantic state enters C. The fixture supplies
bounded memory IO with a captured write/sync trace and uses the actual encoder
callback directly. A separate test-owned shadow arena checks the coordinator's
accepted state after each successful call. Rust compares every shadow checkpoint
and stored segment checkpoint with its own direct original-prefix encoder.
It also checks exact original payload intervals and joins all retained segments.

The five ignored tests cover:

- More than three rotations, an evicted crossing PPU span, and three useful
  complete retained spans with independently expected durations.
- Accepted bootstrap bytes pending in RAM, explicit publication, and empty
  flushes without IO. Metadata exhaustion preserves the accepted HEADER even
  when CLOCKS cannot be admitted.
- Incarnation, descriptor, stack, frame identity, metadata and checkpoint caps,
  including exact-fit success and whole-candidate refusal. Resource admission
  precedes rotation IO; STOPPED_RESOURCE flushes only previously accepted bytes
  and binds the original byte/chunk frontier and reason. Oversized whole payload
  chunks are refused separately.
- Every write and sync observed in a successful baseline, including reuse and
  terminal-root publication, failing independently. Refused candidates preserve
  the accepted arena/frontier; a poisoned handle refuses all later operations
  without IO. A separate immutable snapshot preserves the last known-good root.
- Positive short writes retry; zero progress poisons the writer. Recovery is
  checked independently from acceptance: a failed sync can leave a complete
  newer root visible. This memory fault model makes no power-loss promise.

SEALED and STOPPED_RESOURCE are root-only revisions; they do not add an empty
segment or evict retained evidence. Physical state, original semantic finals,
RAM acceptance and successful persistence remain separate observations.

## Running the integration and sensitivity gates

Run builds in the project's assigned host build slot. Paths below are examples
under that external build root, not installation locations:

```sh
python3 tests/host/ps3dk-prof-rolling/run_ppu_output.py \
  --build-dir "$BUILD/ppu-output"
PS3DK_PROF_ROLLING_PPU_OUTPUT="$BUILD/ppu-output/rolling-ppu-output-fixture" \
  cargo test --manifest-path tools/ps3dk-prof/Cargo.toml \
  --test rolling_ppu_c_output -- --ignored --nocapture
```

The compiler runner accepts `--sanitize address` or `--sanitize undefined` for
separate runs. The C producer has a 120-second per-process test deadline. Source,
trace, checkpoints and both physical snapshots live in unique test-owned
directories; failed tests retain and print them, successful tests remove them.

For sensitivity, build the same Rust test target freshly with `--no-run`; use
the integration-test executable reported by Cargo as `RUST_TEST_BIN` below.
The executable must come from the same checkout state as the fixture. The
runner records its hash and input hashes but does not infer build provenance
from an executable filename.

```sh
python3 tests/host/ps3dk-prof-rolling/run_ppu_output_mutants.py --check-only
python3 tests/host/ps3dk-prof-rolling/run_ppu_output_mutants.py \
  --build-dir "$BUILD/ppu-output-mutants-fresh" \
  --rust-test-bin "$RUST_TEST_BIN"
```

The fresh output directory must be outside the checkout. The runner compiles
only isolated copies of C sources and requires the unchanged five-test baseline
before mutations. Eight controls separately break failed-rotation acceptance,
before-payload checkpoint identity, preflight ordering, accepted-tail flushing,
resource-reason binding, delayed publication, poison refusal and terminal-root
declaration. Each must fail its named semantic assertion in its exact test.
Compiler errors, signals, timeouts, sanitizer failures, unrelated assertions and
surviving mutants fail the gate. A deliberate C semantic assertion is recognized
only when its child process exits normally with status 1 and Rust reports that
exact marker. Source bytes and the prebuilt test binary are checked again after
the run. Build/test logs, hashes and partial results remain on any failure.

The ordinary five-test integration passed the parent-run plain and ASan gates.
Sensitivity results must be recorded from an actual runner invocation; merely
validating its pinned anchors is not evidence that the mutants were detected.
