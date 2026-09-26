#!/usr/bin/env bash
# Vertex texture fetch: TXL, SFL, texture units and sampler records.
#
# Every vertex fetch is TXL, which reads the coordinate and an explicit LOD
# from one source register.  A plain fetch zeroes its LOD lane with SFL; a
# tex2Dbias / tex2Dlod reads the coordinate's w as the LOD.  The unit is two
# bits of hw[2] (8..9).  A vertex sampler takes a texture unit and never a
# c[] register, and implicit units follow FIRST USE.
#
# Expected words, records and compact-CGB entries below were measured on
# the reference compiler (sce-cgc -p sce_vp_rsx) compiling these same
# fixtures.  Rows whose program is a single forced chain (one temp, one
# output) compare the whole ucode; the rest compare the fetch words and the
# unit each output was fetched with, because their unrelated scheduling is
# not what this guards.  Refusals assert exit status 1 AND the named
# diagnostic.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

python3 - "$compiler" "$repo_root/tools/rsx-cg-compiler/tests/shaders" <<'PY'
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

compiler, shaders = sys.argv[1], Path(sys.argv[2])
failures = []
UNDEF = 0xFFFFFFFF


def require(ok, text):
    if not ok:
        failures.append(text)


def compile_fixture(name, out, compact=False):
    src = shaders / (name + '.cg')
    if not src.is_file():
        raise SystemExit('FAIL: fixture missing: %s' % src)
    try:
        return subprocess.run(
            [compiler, '-p', 'sce_vp_rsx',
             '--emit-cgb-container' if compact else '--emit-container',
             str(out), str(src)],
            capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        raise SystemExit('FAIL: %s timed out' % name)


def parse(blob):
    """Records by name, the VP subheader, and the ucode as 4-word tuples."""
    h = struct.unpack_from('>8I', blob, 0)
    records = {}
    for i in range(h[3]):
        row = struct.unpack_from('>12I', blob, h[4] + 48 * i)
        name = blob[row[4]:blob.index(0, row[4])].decode()
        sem = blob[row[7]:blob.index(0, row[7])].decode() if row[7] else ''
        # type, res, resIndex, semantic, paramno, isReferenced, isShared
        records[name] = (row[0], row[1], row[3], sem, row[9], row[10], row[11])
    sub = struct.unpack_from('>6I', blob, h[5])
    words = [struct.unpack_from('>4I', blob, h[7] + 16 * n)
             for n in range(h[6] // 16)]
    return records, sub, words


def compact_entries(blob):
    """Compact CGB LevelB (name, resource) in table order."""
    usize = struct.unpack_from('>H', blob, 8)[0]
    off = 0x20 + usize
    off += struct.unpack_from('>H', blob, off)[0]
    _, count, _ = struct.unpack_from('>HHH', blob, off)
    rows = [struct.unpack_from('>IHH', blob, off + 6 + 8 * i) for i in range(count)]
    strings = blob[off + 6 + 8 * count:]
    return [(strings[o:strings.index(0, o)].decode(), res) for o, _, res in rows]


# --- instruction fields (NV40 VP vector slot) -------------------------------
def vop(w): return (w[1] >> 22) & 0x1F
def sca(w): return (w[1] >> 27) & 0x1F
def to_output(w): return (w[0] >> 30) & 1
def out_reg(w): return (w[3] >> 2) & 0x1F
def dst_temp(w): return (w[0] >> 15) & 0x3F
def mask(w): return ''.join(l for l, b in zip('xyzw', (16, 15, 14, 13)) if (w[3] >> b) & 1)
def unit(w): return (w[2] >> 8) & 3
def src(w, k):
    field = [((w[1] & 0xFF) << 9) | (w[2] >> 23),
             (w[2] >> 6) & 0x1FFFF,
             ((w[2] & 0x3F) << 11) | (w[3] >> 21)][k]
    kind = ('?', 'R', 'IN', 'C')[field & 3]
    swz = ''.join('xyzw'[(field >> s) & 3] for s in (14, 12, 10, 8))
    return kind, (field >> 2) & 0x3F, swz

TXL, SFL, MOV = 0x19, 0x11, 0x01


def listing(words):
    return ' | '.join(' '.join('%08x' % x for x in w) for w in words)


def unit_of_output(words, out):
    """The TXL unit whose result the instruction writing output `out` reads."""
    for i, w in enumerate(words):
        if not (to_output(w) and out_reg(w) == out):
            continue
        temps = {src(w, k)[1] for k in range(3) if src(w, k)[0] == 'R'}
        for p in reversed(words[:i]):
            if vop(p) == TXL and not to_output(p) and dst_temp(p) in temps:
                return unit(p)
    return None


def w(text):
    return tuple(int(x, 16) for x in text.split())


with tempfile.TemporaryDirectory(prefix='ps3dk-vp-texture-fetch-') as tmp:
    root = Path(tmp)

    def build(name, compact=False):
        out = root / (name + ('.cgb' if compact else '.vpo'))
        r = compile_fixture(name, out, compact)
        if r.returncode != 0 or not out.exists():
            failures.append('%s: compile failed (exit %d): %s'
                            % (name, r.returncode, (r.stderr + r.stdout).strip()[-600:]))
            return None
        return out.read_bytes()

    def sampler(records, name, want):
        got = records.get(name)
        require(got == want, '%s record: got %s, want %s' % (name, got, want))

    # ---- whole programs, byte-exact against the reference -----------------
    # (words, registerCount, attributeInputMask)
    exact = {
        # MOV R0.xy, v8.xyxx; SFL R0.z, v8.xxxx, v8.xxxx; TXL R0, R0.xyxz; MOV o0, R0
        'vp_tex_tex2d_v': ([
            w('00001c6c 00400808 0106c083 60419ffc'),
            w('00001c6c 04400800 01000083 60405ffc'),
            w('00001c6c 06400009 0086c083 6041fffc'),
            w('401f9c6c 0040000d 8086c083 6041ff81')], 1, 0x100),
        # TXL R0, v8.xyxw (in place, no SFL); MOV o0, R0
        'vp_tex_tex2dbias_v': ([
            w('00001c6c 06400809 8106c083 6041fffc'),
            w('401f9c6c 0040000d 8086c083 6041ff81')], 1, 0x100),
        'vp_tex_tex2dlod_v': ([
            w('00001c6c 06400809 8106c083 6041fffc'),
            w('401f9c6c 0040000d 8086c083 6041ff81')], 1, 0x100),
        # MOV R0.xy, v8.xyxx; MOV R0.z, c467.xxxx (the LOD; the 3.0 in z is
        # dropped); TXL R0, R0.xyxz; MOV o0, R0
        'vp_tex_lod_construct_v': ([
            w('00001c6c 00400808 0106c083 60419ffc'),
            w('00001c6c 005d3000 0186c083 60405ffc'),
            w('00001c6c 06400009 0086c083 6041fffc'),
            w('401f9c6c 0040000d 8086c083 6041ff81')], 1, 0x100),
        # MOV R0.x, v8.xxxx; SFL R0.y; TXL R0, R0.xxxy; MOV o0, R0
        'vp_tex_tex1d_v': ([
            w('00001c6c 00400800 0106c083 60411ffc'),
            w('00001c6c 04400800 01000083 60409ffc'),
            w('00001c6c 06400000 8086c083 6041fffc'),
            w('401f9c6c 0040000d 8086c083 6041ff81')], 1, 0x100),
        # MOV R0.xyz, v8.xyzx; SFL R0.w; TXL R0, R0.xyzw; MOV o0, R0
        'vp_tex_texcube_v': ([
            w('00001c6c 0040080c 0106c083 6041dffc'),
            w('00001c6c 04400800 01000083 60403ffc'),
            w('00001c6c 0640000d 8086c083 6041fffc'),
            w('401f9c6c 0040000d 8086c083 6041ff81')], 1, 0x100),
    }
    blobs = {}
    for name, (want, regs, inmask) in exact.items():
        blob = build(name)
        if blob is None:
            continue
        blobs[name] = blob
        records, sub, words = parse(blob)
        require(words == want, '%s ucode: got [%s], want [%s]'
                % (name, listing(words), listing(want)))
        require(sub[0] == len(want) and sub[2] == regs and sub[3] == inmask,
                '%s subheader: instructionCount/registerCount/inputMask %s, want %s'
                % (name, (sub[0], sub[2], hex(sub[3])), (len(want), regs, hex(inmask))))
    if 'vp_tex_tex2dbias_v' in blobs and 'vp_tex_tex2dlod_v' in blobs:
        require(parse(blobs['vp_tex_tex2dbias_v'])[2] == parse(blobs['vp_tex_tex2dlod_v'])[2],
                'tex2Dbias and tex2Dlod must be the same vertex instruction')

    # Sampler records: type 0x42a/0x429/0x42d, res 0x800 + unit, resIndex -1,
    # no semantic, paramno -1 (file scope), referenced, not shared.
    for name, rec, typ in [('vp_tex_tex2d_v', 'heightMap', 0x42a),
                           ('vp_tex_tex2dbias_v', 'heightMap', 0x42a),
                           ('vp_tex_lod_construct_v', 'heightMap', 0x42a),
                           ('vp_tex_tex1d_v', 'rampMap', 0x429),
                           ('vp_tex_texcube_v', 'envMap', 0x42d)]:
        if name in blobs:
            sampler(parse(blobs[name])[0], rec, (typ, 0x800, UNDEF, '', UNDEF, 1, 0))
    if 'vp_tex_lod_construct_v' in blobs:
        records = parse(blobs['vp_tex_lod_construct_v'])[0]
        require(records.get('level', (0, 0, 0))[2] == 467,
                'level must keep c467 - the sampler takes no c[] register: %s'
                % (records.get('level'),))
        require(not any(n.startswith('internal-constant-') for n in records),
                'the constructor z lane (3.0) must be dropped, not pooled')

    # ---- the fetch writes only the consumed lanes -------------------------
    blob = build('vp_tex_tex2d_lanes_v')
    if blob is not None:
        _, _, words = parse(blob)
        fetches = [x for x in words if vop(x) == TXL]
        require(fetches == [w('00001c6c 06400009 0086c083 6040bffc')],
                'tex2d_lanes: TXL R0.yw, R0.xyxz expected, got [%s]' % listing(fetches))

    # ---- a temp coordinate is fetched in place ------------------------------
    blob = build('vp_tex_temp_coord_v')
    if blob is not None:
        records, _, words = parse(blob)
        fetches = [x for x in words if vop(x) == TXL]
        zeroes = [x for x in words if vop(x) == SFL]
        require(len(fetches) == 1 and len(zeroes) == 1,
                'temp_coord: one TXL and one SFL expected: [%s]' % listing(words))
        if len(fetches) == 1 and len(zeroes) == 1:
            t, z = fetches[0], zeroes[0]
            coord = src(t, 0)
            require(coord[0] == 'R' and coord[2] == 'xyxz',
                    'temp_coord: TXL must read a temp as .xyxz, got %s' % (coord,))
            require(dst_temp(z) == coord[1] and mask(z) == 'z' and
                    src(z, 0) == ('R', coord[1], 'xxxx') and src(z, 1) == src(z, 0),
                    'temp_coord: SFL must zero z of the coordinate temp and name it .xxxx')
            require(not any(vop(x) == MOV and not to_output(x) and dst_temp(x) == coord[1]
                            for x in words),
                    'temp_coord: the coordinate must not be copied: [%s]' % listing(words))
        require(records.get('offset', (0, 0, 0))[2] == 467,
                'temp_coord: offset must keep c467: %s' % (records.get('offset'),))

    # ---- units: first use, explicit bindings, entry parameters --------------
    cases = {
        'vp_tex_first_use_v': (
            {0: 0, 1: 1},
            {'firstDeclared': (0x42a, 0x801, UNDEF, '', UNDEF, 1, 0),
             'secondDeclared': (0x42a, 0x800, UNDEF, '', UNDEF, 1, 0)},
            {},
            [('uv', 8), ('st', 9), ('firstDeclared', 1), ('secondDeclared', 0)]),
        'vp_tex_units_explicit_v': (
            {0: 2, 1: 3},
            {'heightMap': (0x42a, 0x802, UNDEF, 'TEXUNIT2', UNDEF, 1, 0),
             'maskMap': (0x42a, 0x803, UNDEF, 'TEXUNIT3', UNDEF, 1, 0),
             'spareMap': (0x42a, 0x801, UNDEF, 'TEXUNIT1', UNDEF, 0, 0),
             'farMap': (0x42a, 0xcb8, UNDEF, 'TEXUNIT7', UNDEF, 0, 0),
             'idleMap': (0x42a, 0xcb8, UNDEF, '', UNDEF, 0, 0)},
            {},
            [('uv', 8), ('heightMap', 2), ('maskMap', 3)]),
        'vp_tex_sampler_slots_v': (
            {0: 0, 1: 1},
            {'detailMap': (0x42a, 0x800, UNDEF, '', 1, 1, 0),
             'heightMap': (0x42a, 0x801, UNDEF, '', UNDEF, 1, 0)},
            {'scale': 467, 'tint': 466},
            [('uv', 8), ('detailMap', 0), ('scale', 467), ('heightMap', 1), ('tint', 466)]),
    }
    for name, (units, samplers, uniforms, cgb) in cases.items():
        blob = build(name)
        if blob is not None:
            records, _, words = parse(blob)
            for out, want in units.items():
                got = unit_of_output(words, out)
                require(got == want, '%s: output o%d fetched with unit %s, want %d: [%s]'
                        % (name, out, got, want, listing(words)))
            for rec, want in samplers.items():
                sampler(records, rec, want)
            for rec, reg in uniforms.items():
                got = records.get(rec)
                require(got is not None and got[1] == 0x882 and got[2] == reg,
                        '%s: %s must be c%d (samplers take no c[]): %s' % (name, rec, reg, got))
            require(not any(sca(x) for x in words if vop(x) in (TXL, SFL)),
                    '%s: TXL/SFL must not be co-issued' % name)
        compact = build(name, compact=True)
        if compact is not None:
            got = compact_entries(compact)
            require(got == cgb, '%s compact CGB entries: got %s, want %s' % (name, got, cgb))

    # ---- refusals: exit status exactly 1 and a named diagnostic ------------
    refusals = {
        'vp_tex_five_samplers_refuse_v': ['C6012', "'mapE'"],
        'vp_tex_texunit4_refuse_v': ['C5102', "'heightMap'"],
        'vp_tex_tex2dproj_refuse_v': ['tex2Dproj'],
        'vp_tex_tex3d_refuse_v': ['tex3D'],
        'vp_tex_shadow_refuse_v': ['shadow-compare'],
    }
    for name, needles in refusals.items():
        out = root / (name + '.vpo')
        r = compile_fixture(name, out)
        text = r.stderr + r.stdout
        require(r.returncode == 1,
                '%s: expected exit 1 (a refusal), got %d: %s'
                % (name, r.returncode, text.strip()[-400:]))
        for needle in needles:
            require(needle in text, '%s: diagnostic must name %s: %s'
                    % (name, needle, text.strip()[-400:]))

if failures:
    for f in failures:
        print('FAIL: ' + f, file=sys.stderr)
    sys.exit(1)
PY

printf 'vp-texture-fetch-test: ok\n'
