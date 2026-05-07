# multi-process-xmb — Phase 0 Regression Harness

Test scaffolding for the co-resident VSH+game multi-process refactor
(plan: `~/.claude/plans/multi-process-true-coresident-vsh-and-game.md`).

No RPCS3 source changes. All artifacts live in this directory.

## Scripts

### `smoke_test.sh`

**What:** Boots `hello-spu.fake.self` under the from-source RPCS3 build,
greps the log for three known-good markers:
- `spu_thread_printf` HLE bound (NID `0xb1f4779d`)
- 15 lines of `sys_libc_spu_printf:` success-level output
- Clean SPU exit (`cause=2 status=0 done=12648430`)

**When to run:** After every phase, after any RPCS3 build, before opening a PR.
**Pass:** exit 0, prints `PASS: smoke_test.sh — all markers present`.
**Fail:** exit 1, diagnostic on stderr.

### `perf_baseline.sh`

**What:** Runs `hello-spu.fake.self` N=5 times, captures wall-clock time,
LLVM compile count, and syscall-stats count per run. Computes
median/min/max and writes `baseline.json`.

**When to run:** Once at Phase 0 start, then after every subsequent phase.
Compare `baseline.json` contents across commits to spot regressions.
**Pass:** exit 0, writes `baseline.json` (or a timestamped copy if one exists).
**Fail:** exit 1 if RPCS3 binary or sample is missing.

### `two_process_assert.sh`

**What (Phase 0):** Boots `hello-spu.fake.self`, asserts `sys_process_getpid()`
returns `1` (today's single-process behavior).

**When to run:** Same cadence as smoke_test.
**Future phases:** Extended in-place to add idm namespace checks (P2),
per-process VM isolation (P3), distinct-pid verification (P4+).
**Pass:** exit 0, prints PASS for each phase's assertion block.
**Fail:** exit 1, diagnostic on stderr.

## Dependencies

- Bash (POSIX utilities: grep, awk, sort, head, tail)
- From-source RPCS3 at `/home/firebirdta01/source/repos/RPCS3/build/bin/rpcs3`
- `hello-spu.fake.self` (rebuild via SDK if absent:
  `cd samples/spu/hello-spu && cmake -S . -B build && cmake --build build`)
- `timeout` (GNU coreutils)

No Python, Node, CMake, or jq required.

## Files

| File | Created by | Description |
|------|-----------|-------------|
| `smoke_test.sh` | Phase 0 | Boot + log-marker assertions |
| `perf_baseline.sh` | Phase 0 | 5-run perf capture → `baseline.json` |
| `two_process_assert.sh` | Phase 0 | Phase-evolving multi-process assertions |
| `baseline.json` | `perf_baseline.sh` | Phase 0 perf baseline (git-rev tagged) |
| `README.md` | Phase 0 | This file |
