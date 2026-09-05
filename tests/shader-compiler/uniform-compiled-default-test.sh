#!/usr/bin/env bash
# A file-scope uniform declared with an initialiser carries that value as
# its compiled default, in the const block a runtime patch overwrites, and
# stays patchable (t_3bf3ce95).  Checked in the container: the defect
# produced a well-formed, patchable parameter over a zero block, so
# compiling successfully proves nothing.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_uniform_default_f.cg"
work="${TMPDIR:-/tmp}/ps3dk-uniform-default-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# BOTH lowering paths: the value belongs to the parameter, so a fix that
# only reached one path would leave the other computing with zero.
# Shelf-life: when the retired legacy matcher is removed, drop this second
# --legacy-lowering run and its header claim in the same commit.
for path in general legacy; do
    flags=()
    [[ "$path" == legacy ]] && flags=(--legacy-lowering)
    rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "${flags[@]}" \
            --emit-container "$work/$path.fpo" "$src"
    ) >"$work/$path.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "fp_uniform_default ($path) timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$path.log" >&2
        fail "fp_uniform_default failed to compile on the $path path"
    fi

    python3 - "$work/$path.fpo" "$path" <<'PY'
import struct, sys

blob, path = open(sys.argv[1], "rb").read(), sys.argv[2]
u32 = lambda o: struct.unpack_from(">I", blob, o)[0]
unswap = lambda v: ((v << 16) | (v >> 16)) & 0xFFFFFFFF

def cstr(o):
    return "" if not o else blob[o:blob.index(b"\0", o)].decode("ascii", "replace")

def block_at(off):
    raw = blob[u32(28) + off : u32(28) + off + 16]
    words = [unswap(struct.unpack_from(">I", raw, 4 * i)[0]) for i in range(4)]
    return [round(struct.unpack(">f", struct.pack(">I", w))[0], 6) for w in words]

want = {
    "tint":  [0.25, 0.5, 0.75, 1.0],   # explicit four-component
    "splat": [0.5, 0.5, 0.5, 0.5],     # SCALAR broadcast across the vector
    "plain": [0.0, 0.0, 0.0, 0.0],     # no initialiser: stays zero
}
seen = {}
for i in range(u32(12)):
    base = u32(16) + i * 48
    name = cstr(u32(base + 16))
    if name not in want:
        continue
    emb = u32(base + 24)
    if not emb:
        raise SystemExit(
            "FAIL(%s): uniform '%s' has no embedded-constant record, so it is "
            "not patchable at runtime" % (path, name)
        )
    seen[name] = True
    for n in range(u32(emb)):
        got = block_at(u32(emb + 4 + 4 * n))
        if got != want[name]:
            raise SystemExit(
                "FAIL(%s): uniform '%s' const block is %s, expected %s"
                % (path, name, got, want[name])
            )

missing = sorted(set(want) - set(seen))
if missing:
    raise SystemExit(
        "FAIL(%s): never examined %s - the assertions above prove nothing "
        "if the parameters are absent" % (path, ", ".join(missing))
    )
PY
done

printf 'uniform-compiled-default-test: ok\n'
