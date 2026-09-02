#!/usr/bin/env bash
# A float4 literal keeps all four lanes in the vertex constant pool
# (t_3e342903).  The general path's pool read literal[0] and nothing else,
# so every literal became one packed lane read back with an .xxxx
# broadcast: `out = float4(a,b,c,d)` painted `a` four times, silently.
#
# Asserted on the CONTAINER rather than the ucode, because the defect is
# in what the container carries: the internal-constant parameters are the
# literal pool, and their declared type and default values are exactly the
# thing that went missing.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/vp_literal_vector_v.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

work="${TMPDIR:-/tmp}/ps3dk-vp-literal-vector-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_vp_rsx --general-lowering \
        --emit-container "$work/out.vpo" "$src"
) >"$work/out.log" 2>&1 || rc=$?
[[ "$rc" -eq 124 ]] && fail "vp_literal_vector timed out"
if [[ "$rc" -ne 0 ]]; then
    tail -n 20 "$work/out.log" >&2
    fail "vp_literal_vector failed to compile on the general path"
fi

python3 - "$work/out.vpo" <<'PY'
import struct
import sys

blob = open(sys.argv[1], "rb").read()
u32 = lambda o: struct.unpack_from(">I", blob, o)[0]
f32 = lambda o: struct.unpack_from(">f", blob, o)[0]


def cstr(o):
    return "" if not o else blob[o:blob.index(b"\0", o)].decode("ascii", "replace")


CG_FLOAT4 = 1048
pcount, parr = u32(12), u32(16)

# Parameter record: type, res, var, resIndex, nameOffset, defaultValue, ...
slots = []
for i in range(pcount):
    base = parr + i * 48
    name = cstr(u32(base + 16))
    if not name.startswith("internal-constant"):
        continue
    default = u32(base + 20)
    if not default:
        raise SystemExit(
            "FAIL: %s carries no default-value block, so the literal it "
            "stands for has no value in the container" % name
        )
    slots.append((name, u32(base), [f32(default + 4 * k) for k in range(4)]))

if len(slots) != 2:
    raise SystemExit(
        "FAIL: the pool must hold exactly two slots - two distinct float4 "
        "literals, the third store repeating the first - and holds %d: %s.  "
        "One slot means every literal was packed into lanes of a shared "
        "register and broadcast (t_3e342903); three means the duplicate was "
        "not shared." % (len(slots), ", ".join(s[0] for s in slots))
    )

want = {
    (0.404, 0.186, 0.277, 0.349),
    (0.754, 0.027, -0.061, -0.054),
}
got = set()
for name, cgtype, values in slots:
    if cgtype != CG_FLOAT4:
        raise SystemExit(
            "FAIL: %s is declared as CGtype %d, not float4 (%d).  The "
            "declared width comes from the slot's used lanes, so anything "
            "narrower means lanes of the literal were dropped."
            % (name, cgtype, CG_FLOAT4)
        )
    got.add(tuple(round(v, 6) for v in values))

if got != want:
    raise SystemExit(
        "FAIL: the pool holds %s; the source's two distinct literals are %s"
        % (sorted(got), sorted(want))
    )
PY

printf 'vp-literal-vector-test: ok\n'
