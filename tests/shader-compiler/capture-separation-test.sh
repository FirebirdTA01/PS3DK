#!/usr/bin/env bash
# A ucode dump that a test DECODES must never be captured with the streams
# merged.
#
# WHY.  The compiler prints the ucode rows on stdout and its diagnostics -
# including the RSX_DUMP_ORDER trace some of these tests also read - on
# stderr.  `>"$log" 2>&1` interleaves them, and under WSL a stderr line
# landed INSIDE a hex row: the row failed to parse, every later row shifted
# by one, and a constant decoded as an instruction writing R33, a register
# nothing reads.  colour-reaches-r0-test.sh reported that as a dead write
# the compiler had never emitted, and two people spent an hour comparing
# four accumulation_mad rows in opposite environments before the cause was
# isolated (2026-09-07).
#
# The shared decoder now REFUSES a log it cannot trust, which turns that
# into a loud failure instead of a wrong finding.  This guard is the other
# half: it keeps the logs trustworthy in the first place, and it keeps them
# that way as tests are added, because the refusal only fires where someone
# happens to run the suite in the environment that corrupts.
#
# The check is static and it is a HEURISTIC, stated plainly because a guard
# that oversells itself is worse than a narrow one: it looks for a
# redirection that merges the streams into a file, inside a script that
# mentions ucode_decode, and flags it when "$compiler" appears in the eight
# lines before it.  It does NOT prove that the merged file is the one the
# decoder reads - matching a capture to its consumer would need to follow a
# shell variable through the script.  What that costs is precision, not
# soundness: a merged capture of the compiler in a decoding test gets
# flagged whether or not that particular log is decoded, and a merged
# capture of a python helper (fp-sources' own decoder output) is not
# flagged, which is the one exclusion the rule intends.  Every capture the
# rule reports is a real merged compiler capture; some of them may be
# harmless.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$repo_root/tests/shader-compiler"

python3 - "$PWD" <<'PY'
import glob
import io
import os
import re
import sys

# A capture that merges the streams INTO A FILE.  >/dev/null is not one.
MERGED = re.compile('>[ \t]*"([^"]*)"[ \t]*2>&1')
# A script that reads ucode rows: it imports the shared decoder or runs it.
DECODES = re.compile(r'ucode_decode')

bad = []
checked = []
for path in sorted(glob.glob(os.path.join(sys.argv[1], "*.sh"))):
    name = os.path.basename(path)
    if name == "capture-separation-test.sh":
        continue                      # its own regex would match itself
    text = io.open(path, encoding="utf-8", errors="replace").read()
    if not DECODES.search(text):
        continue
    lines = text.splitlines()
    checked.append(name)
    for lineno, line in enumerate(lines, 1):
        m = MERGED.search(line)
        if not m or m.group(1) == "/dev/null":
            continue
        # Only a capture OF THE COMPILER matters: the dump is what gets
        # decoded.  A python helper's own output may be merged - it is read
        # by a human when the test fails, not by the decoder.  Look back
        # through the enclosing command for the invocation.
        window = lines[max(0, lineno - 8):lineno]
        joined = chr(10).join(window)
        if "$compiler" not in joined:
            continue
        bad.append((name, lineno, m.group(1), line.strip()[:70]))

if len(checked) < 10:
    raise SystemExit("FAIL: only %d decoding tests were examined (%s) - the "
                     "enumeration broke, so nothing above was checked"
                     % (len(checked), ", ".join(checked)))

if bad:
    for name, lineno, target, text in bad:
        print("  %s:%d captures the compiler into %s with 2>&1"
              % (name, lineno, target), file=sys.stderr)
    raise SystemExit(
        "FAIL: %d merged compiler capture(s) in tests that decode ucode.  A "
        "stderr line landing inside a hex row costs the row and shifts every "
        "later one, which is how a harness problem becomes a compiler "
        "finding.  Capture stdout and stderr into separate files and read the "
        "diagnostics from the stderr half." % len(bad))

print("capture-separation: %d tests decode ucode, none captures the compiler "
      "with the streams merged" % len(checked))
PY

printf 'capture-separation: PASS\n'
