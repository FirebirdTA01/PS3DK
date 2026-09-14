# Uniform container cross-check

`uniform_container_check.check_container(blob)` returns a JSON-compatible report
with `profile`, `checks`, `issues`, `reads`, and `inline_blocks`. Each check names
the record and parameter number, a stable diagnostic code, its evidence and one
of `verified`, `mismatch`, `unresolved`, or `not_applicable`. Malformed containers
produce `malformed_container`; they do not turn into empty successful reports.

Run a single container:

```sh
python3 tests/shader-compiler/uniform_container_check.py shader.fpo
```

The CLI exits 0 without findings, 1 for mismatches or unresolved coverage, and 2
for malformed input. The function preserves the full report in all three cases.
An optional `expected_profile` argument checks the caller's requested profile;
the corpus driver supplies it on every accepted compile.

For FP, the checker walks instructions using opcode source arities, identifies
the inline blocks they consume, and validates every embedded-constant offset.
When a record has a default, all four raw float words must match every listed
inline occurrence. Only the instruction-side words are halfword-unswapped.
This includes padding, negative zero and NaN payloads; there is no tolerance.
A defaultless record can legitimately have embedded offsets: those are runtime
patch locations, independent of whether an initializer was declared.

For VP, both vector and scalar slots contribute their actual constant operands.
Unused encoded source slots do not count. Every direct read must resolve to a
referenced leaf record; `CG_CONSTANT` literal-pool records count too. Referenced
scalar/vector records require a read, except where indirect or unknown encoding
prevents that conclusion. Explicit aliases are legal. Matrix parent and row-zero
records may share a register. Row reference flags have aggregate granularity:
the reference marks `m3[0]` and `m3[1]` referenced in `vp_matrix_row_small_v.cg`
although only `m3[2]` is read. An unread matrix row therefore does not require an
individual read. Actual reads still need a declared leaf.

Limits are part of the result:

- FP explicit C-register bindings are metadata, not physical C-register reads.
- VP defaults are uploaded by the runtime; there is no inline duplicate.
- An unreferenced default with no embedded occurrences has no instruction-side
  copy to compare. A referenced FP default with no occurrences is unresolved,
  never a clean not-applicable result.
- Input-dependent VP relative addresses remain unresolved. Matrix-array
  addressing is no more resolved than vector-array addressing; matrix row
  liveness being aggregate merely removes an invalid per-row requirement.
- A permutation of uniform identities preserving the register set is invisible
  to structural membership. The self-test demonstrates this limit with a real
  mutated container and catches it separately with independent, asymmetric
  register values through `vp_binding_check.evaluate_bindings`.

## Corpus guard and baseline

`uniform-container-crosscheck-test.sh COMPILER` first runs real-container and
mutation checks through the same callable, then compiles the tracked corpus.
It uses the existing `_v.cg` / `.vcg` VP naming convention. Every other tracked
shader runs as FP. Compile refusals, crashes and timeouts are separate outcomes;
only exit 1 is a refusal. SDK coverage is explicitly reported as skipped in CI.
The local driver can additionally take `--sdk-csv`, `--sdk-root` and another
`--allowlist`; private corpus results and SDK allowances are kept outside Git.

`uniform_container_allowlist.json` is a **day-one baseline**, not an allowance
for growth. Each entry pins source/profile/record/parameter number, diagnostic,
concrete register and instruction offset/source slot where available, count,
card and reason. A new or changed finding fails, as does an obsolete allowance.
No default/inline mismatch or orphan read is allowed by the initial baseline.
Unused scalar/vector metadata is tracked by t_666de9fd; unresolved indirect
address interpretation is t_269f03d7. Updating the baseline requires review of
the measured entries, not regenerating it to make a failing run green.

For a durable local census, select a fresh output directory:

```sh
python3 tests/shader-compiler/uniform_container_census.py /path/to/compiler \
  --output /path/to/new-census \
  --allowlist tests/shader-compiler/uniform_container_allowlist.json
```

The driver saves incremental `outcomes.jsonl`, container/log artifacts and a
final `summary.json`, and prints progress every 25 sources. A populated output
directory is not reused, so a stale row cannot be mistaken for a new result.
