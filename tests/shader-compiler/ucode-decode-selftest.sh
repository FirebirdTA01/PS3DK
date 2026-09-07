#!/usr/bin/env bash
# The shared ucode decoder must REFUSE a log that is not a faithful copy of
# the dump, rather than silently decoding a shorter, misaligned program.
#
# WHY THIS EXISTS.  Capturing the compiler with a merged `2>&1` lets a
# stderr line interleave INSIDE a hex row.  The row then fails the parse,
# groups() used to drop it silently, every later row shifted by one, and a
# constant got decoded as an instruction writing a register nothing reads -
# a "dead write" the compiler never emitted.  On 2026-09-07 that had two
# people comparing four accumulation_mad rows for an hour: one ran the suite
# under WSL with merged capture and saw the dead writes, the other ran it
# natively with separate capture and saw none, ON THE SAME BINARY.  The
# compiler was never wrong; the log was.
#
# A decoder that silently produces a plausible answer from a corrupted input
# is worse than one that fails, because every guard built on it inherits the
# corruption as a finding.  So the corruption is now detectable and named:
# rows are NUMBERED, and a gap or an unparseable row is proof of loss.
#
# This guard is adversarial by construction - it CORRUPTS a real dump in the
# three ways that actually occur and requires the decoder to refuse each -
# and it pins the good cases too, so a decoder that simply rejected
# everything would fail here rather than pass quietly.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-ucode-decode-selftest.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
src="$shaders/fp_half_output_partial_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

# A REAL dump, captured with stdout and stderr kept apart - which is also
# the capture discipline this guard is asking every other test to adopt.
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" -p sce_fp_rsx "$src"
) >"$work/good.log" 2>"$work/good.err" || {
    tail -n 5 "$work/good.err" >&2
    fail "the fixture did not compile, so there is no dump to corrupt"
}

python3 - "$repo_root/tests/shader-compiler" "$work" <<'PY'
import io
import re
import sys

sys.path.insert(0, sys.argv[1])
from ucode_decode import groups, decode

# Deliberately NOT importing CorruptDump by name.  A guard that fails with
# ImportError on a decoder lacking the symbol proves only that a symbol is
# missing; this one has to fail because the decoder SILENTLY DECODES A
# MISALIGNED PROGRAM, which is the actual defect.  Any exception counts as a
# refusal; returning a short list does not.
try:
    from ucode_decode import CorruptDump
except ImportError:
    CorruptDump = ()

work = sys.argv[2]
good = io.open("%s/good.log" % work, encoding="utf-8", errors="replace").read().splitlines()

rows = [n for n, l in enumerate(good) if re.match(r"\s*\d+:\s+[0-9a-fA-F]{8}", l)]
if len(rows) < 3:
    raise SystemExit("FAIL: the fixture produced %d dump rows, too few to corrupt "
                     "meaningfully - the guard would prove nothing" % len(rows))

def write(name, lines):
    p = "%s/%s.log" % (work, name)
    io.open(p, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    return p

def must_refuse(name, lines, why, clean_rows):
    p = write(name, lines)
    try:
        got = groups(p)
    except Exception:
        return                      # refused, by any means: correct
    raise SystemExit(
        "FAIL: %s - the decoder ACCEPTED a corrupted dump and returned %d of "
        "%d rows (%s).  Silently decoding a misaligned program is how a "
        "harness problem becomes a compiler finding: every row after the lost "
        "one shifts, and a constant gets read as an instruction writing a "
        "register nothing reads." % (name, len(got), clean_rows, why))

# 1. THE GOOD CASE still decodes, so a decoder that refuses everything fails.
p = write("clean", good)
n_clean = len(groups(p))
if n_clean != len(rows):
    raise SystemExit("FAIL: the clean dump decoded %d of %d rows"
                     % (n_clean, len(rows)))
if not list(decode(p)):
    raise SystemExit("FAIL: the clean dump decoded to no instructions")

# 2. A STDERR LINE SPLICED INSIDE A ROW - the exact shape that occurred.
spliced = []
for n, line in enumerate(good):
    if n == rows[1]:
        half = len(line) // 2
        spliced.append(line[:half] + "  src0 kind=1 idx=116 phys=-1")
        spliced.append(line[half:])
    else:
        spliced.append(line)
must_refuse("spliced", spliced, "a stderr fragment landed inside a hex row", len(rows))

# 3. A ROW SIMPLY MISSING - same end state, different cause (a lost write,
#    a truncated pipe).  The row numbers are what make this detectable.
dropped = [l for n, l in enumerate(good) if n != rows[1]]
must_refuse("dropped", dropped, "a whole row went missing", len(rows))

# 4. THE SAME SPLICE ON THE **LAST** ROW, landing before the first word.
#    This is the shape the index sequence CANNOT see: a lost final row has
#    no successor index to disagree with, so the run-length check stays
#    silent and the decoder returns n-1 of n rows as if that were the whole
#    program.  The unparseable row therefore has to raise ON ITS OWN.
#    codex measured this against both decoders on a real 174-row dump
#    (build/codex-dump-integrity-review/last-row-spliced.log): both returned
#    173 of 174 before this rule.
last = []
for n, line in enumerate(good):
    if n == rows[-1]:
        head, _, tail = line.partition(":")
        last.append(head + ":  src0 kind=1 idx=116 phys=-1")
        last.append(tail)
    else:
        last.append(line)
must_refuse("last-row", last, "a stderr fragment landed in the FINAL row, "
            "where no later index can expose the loss", len(rows))

# 4. A MULTI-PROGRAM LOG must still decode: indices restart at 0, and that
#    restart must not be mistaken for a gap.
both = good + good
p = write("twice", both)
if len(groups(p)) != 2 * len(rows):
    raise SystemExit("FAIL: two dumps in one log decoded %d rows, expected %d - "
                     "the restart-at-zero rule is wrong"
                     % (len(groups(p)), 2 * len(rows)))

print("ucode-decode-selftest: clean %d rows, spliced / dropped / final-row "
      "all refused, two-program log %d rows" % (n_clean, 2 * len(rows)))
PY

printf 'ucode-decode-selftest: PASS\n'
