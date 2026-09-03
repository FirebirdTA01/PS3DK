#!/usr/bin/env bash
# vpOpcode must not fall through to MOV for unknown VOp values.
#
# The old helper returned VP_OP(MOV) from its default path.  That made any
# newly-added VOp that accidentally reached vertex emission a silent MOV in a
# vertex program.  DIVR is the immediate reason this guard exists, but the
# rule is wider than DIVR: unsupported VP operations must refuse by name before
# encoding, not inherit a plausible-looking arithmetic instruction.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
source_file="$repo_root/tools/rsx-cg-compiler/src/nv40/nv40_general_lowering.cpp"

python3 - "$source_file" <<'PY'
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()
legacy = re.search(
    r"static\s+uint8_t\s+vpOpcode\s*\([^)]*\)\s*\{(?P<body>.*?)\n\}",
    src, re.S)
if legacy:
    body = legacy.group("body")
    if re.search(r"\n\s*return\s+VP_OP\s*\(\s*MOV\s*\)\s*;\s*$", body):
        raise SystemExit(
            "FAIL: vpOpcode still falls through to VP_OP(MOV).  Unknown VOp "
            "values must refuse, not silently encode as MOV.")
else:
    guarded = re.search(
        r"static\s+bool\s+tryVpOpcode\s*\([^)]*\)\s*\{(?P<body>.*?)\n\}",
        src, re.S)
    if not guarded:
        raise SystemExit(
            "FAIL: no vpOpcode or tryVpOpcode helper found to audit")
    if "default: return false;" not in guarded.group("body"):
        raise SystemExit(
            "FAIL: tryVpOpcode exists but its default is not a refusal")
if re.search(r"\n\s*return\s+VP_OP\s*\(\s*MOV\s*\)\s*;\s*\n\}", src):
    raise SystemExit(
        "FAIL: a VP opcode helper still falls through to VP_OP(MOV).  Unknown "
        "VOp values must refuse, not silently encode as MOV.")
PY

printf 'PASS: vp-opcode-fallthrough-test\n'
