#!/usr/bin/env bash
# gcm-forwarder-ratchet-test.sh — API-rule ratchet on the cellGcm forward
# surface in <cell/gcm.h> and <cell/gcm/gcm_command_c.h>.
#
# A "forwarder" is a cellGcm* static-inline whose body calls a PSL1GHT
# (rsx*/gcm*) function. As cellGcm* natives are added (logic-op/depth/blend/
# fog/AA/clip/... in <cell/gcm/gcm_state_native.h>), those forwarders convert
# their bodies to ps3tc_gcm_state_* natives and DROP off the forwarder list.
# The allowlist in gcm-forwarder-allowlist.txt must therefore SHRINK across
# commits.
#
# Fails when:
#   * a NEW cellGcm* forwarder appears (a shim regressed), or
#   * an allowlisted name is no longer a forwarder (converted but not
#     removed from the allowlist), so the list must shrink as functions
#     are converted.
#
# Not a per-function body check — that's the role of gcm-state-native-test
# and gcm-state-cellpath-test. This test is on the FORWARDER SURFACE.

set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)

ALLOWLIST="$root/tests/sdk/gcm-forwarder-allowlist.txt"
CMDC="$root/sdk/include/cell/gcm/gcm_command_c.h"
GCMH="$root/sdk/include/cell/gcm.h"

for f in "$ALLOWLIST" "$CMDC" "$GCMH"; do
    [ -f "$f" ] || { echo "gcm-forwarder-ratchet: FAIL: missing ${f}" >&2; exit 1; }
done
[ -s "$ALLOWLIST" ] || { echo "gcm-forwarder-ratchet: FAIL: allowlist is empty" >&2; exit 1; }

python3 - "$ALLOWLIST" "$CMDC" "$GCMH" <<'PY'
import re, sys

allow_path, ccmd_path, gcmh_path = sys.argv[1:4]

def body_and_name(path):
    """Yield (cellGcmName, body) for every cellGcm* static-inline function."""
    text = open(path).read()
    out = []
    for m in re.finditer(r'static inline [^\n;]*?\b(cellGcm[A-Z]\w*)\s*\(', text):
        name = m.group(1)
        i = m.end()
        # walk off the parameter list (balanced parens)
        depth_paren = 1
        while i < len(text) and depth_paren:
            if text[i] == '(': depth_paren += 1
            elif text[i] == ')': depth_paren -= 1
            i += 1
        # skip whitespace up to the opening brace
        while i < len(text) and text[i] in ' \t\n\r': i += 1
        if i >= len(text) or text[i] != '{':
            continue
        i += 1
        depth_curly = 1
        start = i
        while i < len(text) and depth_curly:
            if text[i] == '{': depth_curly += 1
            elif text[i] == '}': depth_curly -= 1
            i += 1
        out.append((name, text[start:i]))
    return out

def is_forward(body):
    # calls rsx<Cap>...( or gcm<Cap>...(.  Exclude cellGcm* (self/peer) and
    # ps3tc_* (our own natives — those are CONVERTED, not forwards).
    return bool(re.search(
        r'(?<!cell)(?<!ps3tc_)(?<![A-Za-z0-9_])(rsx[A-Z]\w*|gcm[A-Z]\w*)\s*\(', body))

actual = set()
for path in (ccmd_path, gcmh_path):
    for name, body in body_and_name(path):
        if is_forward(body):
            actual.add(name)

allowed = [l.strip() for l in open(allow_path)
           if l.strip() and not l.startswith('#')]

new      = sorted(actual - set(allowed))
missing  = sorted(set(allowed) - actual)

print(f"cellGcm forwarder surface: {len(actual)} actual / {len(allowed)} allowed")
if new:
    print("FAIL: NEW forwarders (regressed to a shim — convert to a native or explain):")
    for n in new: print(f"  + {n}")
if missing:
    print("FAIL: allowlist names no longer forward (converted — remove from the allowlist):")
    for n in missing: print(f"  - {n}")
if new or missing:
    print("\nUpdate tests/sdk/gcm-forwarder-allowlist.txt and re-run.")
    sys.exit(1)
print("PASS (forwarder surface unchanged; allowlist is in sync — the list only shrinks)")
PY
