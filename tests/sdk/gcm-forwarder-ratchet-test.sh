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
#
# Usage:
#   gcm-forwarder-ratchet-test.sh [--root <dir>] [--self-test]
#     --root <dir>  Scan the tree under <dir> instead of the repo root.
#     --self-test   Run the scanner on a bundle of synthetic trees that cover
#                   every failure class (same-line new forwarder; MULTILINE new
#                   forwarder, which a same-line-only scanner missed; pointer
#                   return type; allowlisted-but-converted; attributes, comments
#                   and braces in comments or strings around a definition) plus
#                   clean trees. Each rejecting case must exit 1 AND name the offending
#                   function; the clean tree must exit 0.

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)

# ---- --root override (used by the self-test harness) ---------------------
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --root)      shift; args+=("$1"); break ;;
        --self-test) SELFTEST_MODE=1; shift ;;
        *) shift ;;
    esac
done
[ ${#args[@]} -eq 1 ] && root="$1"

ALLOWLIST="$root/tests/sdk/gcm-forwarder-allowlist.txt"
CMDC="$root/sdk/include/cell/gcm/gcm_command_c.h"
GCMH="$root/sdk/include/cell/gcm.h"

# ---- --self-test: run the whole battery on synthetic trees --------------
if [ "${SELFTEST_MODE:-0}" = "1" ]; then
    python3 - "$root" <<'SELFTEST'
import os, sys, subprocess, tempfile, textwrap

repo_root = sys.argv[1]
SCRIPT    = os.path.join(repo_root, "tests", "sdk", "gcm-forwarder-ratchet-test.sh")

def build_tree(tmpdir, ccmd_text, allowlist_text):
    os.makedirs(os.path.join(tmpdir, "tests", "sdk"),                          exist_ok=True)
    os.makedirs(os.path.join(tmpdir, "sdk", "include", "cell", "gcm"),          exist_ok=True)
    with open(os.path.join(tmpdir, "tests", "sdk", "gcm-forwarder-allowlist.txt"), "w") as f:
        f.write(allowlist_text)
    with open(os.path.join(tmpdir, "sdk", "include", "cell", "gcm", "gcm_command_c.h"), "w") as f:
        f.write(ccmd_text)
    with open(os.path.join(tmpdir, "sdk", "include", "cell", "gcm.h"), "w") as f:
        f.write("")

def run(root):
    p = subprocess.run(["bash", SCRIPT, "--root", root],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr

failures = []

# Every tree carries one allowlisted forwarder, so the allowlist is never empty and a rejection can only come from
# the scan itself; a rejecting case must also name the function it rejects.
KNOWN = "static inline void cellGcmSetKnown(CellGcmContextData *c) { rsxSetFogMode(c,1); }\n"

def case(label, ccmd, allowlist, want_rc, want_text=None):
    with tempfile.TemporaryDirectory() as tmp:
        build_tree(tmp, KNOWN + ccmd, allowlist)
        rc, out = run(tmp)
    ok = (rc == want_rc) and (want_text is None or want_text in out)
    print(f"  {label:38s} -> rc={rc} (want {want_rc})  {'ok' if ok else 'FAIL'}")
    if not ok:
        failures.append(f"{label}: rc={rc} want {want_rc}\n{out}")

# 1. Same-line NEW forwarder (not in allowlist) → FAIL (rc=1)
case("1 same-line new fwd",
     "static inline void cellGcmSetNewForward(CellGcmContextData *c) { rsxSetFogMode(c,1); }\n",
     "cellGcmSetKnown\n", 1, "+ cellGcmSetNewForward")

# 2. MULTILINE NEW forwarder (the regression class a same-line scanner missed) → FAIL (rc=1)
case("2 multiline new fwd",
     "static inline void\n"
     "cellGcmSetMultilineNewForward(CellGcmContextData *c)\n"
     "{ rsxSetFogMode(c,1); }\n",
     "cellGcmSetKnown\n", 1, "+ cellGcmSetMultilineNewForward")

# 3. Pointer-return NEW forwarder (multiline) → FAIL (rc=1)
case("3 pointer-return new fwd",
     "static inline uint32_t *\n"
     "cellGcmSetPtrNewForward(CellGcmContextData *c)\n"
     "{ rsxSetFogMode(c,1); return 0; }\n",
     "cellGcmSetKnown\n", 1, "+ cellGcmSetPtrNewForward")

# 4. Allowlisted but CONVERTED (was a forwarder, now a native call) → FAIL (rc=1)
case("4 allowlisted-but-converted",
     "static inline void cellGcmSetConverted(CellGcmContextData *c) { ps3tc_gcm_state_converted(c); }\n",
     "cellGcmSetKnown\ncellGcmSetConverted\n", 1, "- cellGcmSetConverted")

# 5. Clean tree: one forwarder, allowlisted → PASS (rc=0)
case("5 clean tree (PASS)", "", "cellGcmSetKnown\n", 0)

# 6. Same-line new forwarder when allowlist is non-trivial → FAIL (rc=1)
case("6 second new fwd beside a known one",
     "static inline void cellGcmSetAdded(CellGcmContextData *c) { rsxSetFogMode(c,1); }\n",
     "cellGcmSetKnown\n", 1, "+ cellGcmSetAdded")

# 7-11. Valid definitions a syntax-matching scanner missed (review 2026-11-12): an attribute before the name,
# a comment between the parameter list and the body, a closing brace inside a body comment or a string, and an
# attribute after the parameter list. Each is a new forwarder and must be rejected by name.
case("7 attribute before the name",
     "static inline __attribute__((always_inline)) void cellGcmSetProbeA(void *c)\n{ rsxSetFogMode(c,1); }\n",
     "cellGcmSetKnown\n", 1, "+ cellGcmSetProbeA")
case("8 comment before the body",
     "static inline void cellGcmSetProbeB(void *c) /* emit fog mode */\n{ rsxSetFogMode(c,1); }\n",
     "cellGcmSetKnown\n", 1, "+ cellGcmSetProbeB")
case("9 closing brace in a body comment",
     "static inline void cellGcmSetProbeC(void *c)\n{ /* close } is a comment */ rsxSetFogMode(c,1); }\n",
     "cellGcmSetKnown\n", 1, "+ cellGcmSetProbeC")
case("10 closing brace in a string",
     "static inline void cellGcmSetProbeD(void *c)\n{ const char *s = \"}\"; (void)s; rsxSetFogMode(c,1); }\n",
     "cellGcmSetKnown\n", 1, "+ cellGcmSetProbeD")
case("11 attribute after the parameters",
     "static inline void cellGcmSetProbeE(void *c) __attribute__((always_inline))\n{ rsxSetFogMode(c,1); }\n",
     "cellGcmSetKnown\n", 1, "+ cellGcmSetProbeE")
# 12. A forwarder call that appears only in a comment is not a forward.
case("12 rsx call only in a comment",
     "static inline void cellGcmSetCommented(void *c) { /* rsxSetFogMode(c,1); */ (void)c; }\n",
     "cellGcmSetKnown\n", 0)

if failures:
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    print("FAIL (self-test: %d case(s) misbehaved)" % len(failures))
    sys.exit(1)
print("PASS (all ratchet self-test cases behave correctly)")
SELFTEST
    exit $?
fi

# ---- normal scan ---------------------------------------------------------
for f in "$ALLOWLIST" "$CMDC" "$GCMH"; do
    [ -f "$f" ] || { echo "gcm-forwarder-ratchet: FAIL: missing ${f}" >&2; exit 1; }
done
[ -s "$ALLOWLIST" ] || { echo "gcm-forwarder-ratchet: FAIL: allowlist is empty" >&2; exit 1; }

python3 - "$ALLOWLIST" "$CMDC" "$GCMH" <<'PY'
import re, sys

allow_path, ccmd_path, gcmh_path = sys.argv[1:4]

# Definitions are found lexically, not by matching the declaration syntax:
# comments, string and character literals are blanked first (newlines kept),
# then every `cellGcm<Cap>...(` whose balanced parameter list is followed,
# after any `__attribute__((...))` / `__asm__(...)` suffixes, by `{` is a
# definition, whatever precedes the name (static inline, attributes, a
# multiline or pointer return type). A call inside a body is followed by
# `;`, `)` or an operator, never by `{`, so it is not taken for one. Bodies
# are delimited on the blanked text, so a brace inside a comment or string
# cannot end a body early.
_LEX = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|' + r"'(?:\\.|[^'\\\n])*'", re.S)

def _blank(text):
    """text with comments and literals replaced by spaces (newlines kept)."""
    return _LEX.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text)

NAME_RE = re.compile(r'\b(cellGcm[A-Z]\w*)\s*\(')
SUFFIX_RE = re.compile(r'\s*(?:__attribute__|__asm__|asm)\s*\(')

def _end(text, i, o, c):
    """text[i-1]==o; return index just past the matching c."""
    depth = 1
    while i < len(text) and depth:
        if text[i] == o: depth += 1
        elif text[i] == c: depth -= 1
        i += 1
    return i

def body_and_name(path):
    """Return [(cellGcmName, body)] for every cellGcm* function definition."""
    text = _blank(open(path).read())
    out = []
    seen = set()
    for m in NAME_RE.finditer(text):
        name = m.group(1)
        if name in seen:
            continue
        i = _end(text, m.end(), '(', ')')          # just past the parameter list
        while True:                                # attribute / asm suffixes
            s = SUFFIX_RE.match(text, i)
            if not s: break
            i = _end(text, s.end(), '(', ')')
        while i < len(text) and text[i] in ' \t\n\r': i += 1
        if i >= len(text) or text[i] != '{':
            continue                               # a call or a declaration
        close = _end(text, i + 1, '{', '}')
        seen.add(name)
        out.append((name, text[i + 1 : close - 1]))
    return out

def is_forward(body):
    # Calls rsx<Cap>...( or gcm<Cap>...(.  Exclude cellGcm* (self/peer) and
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
