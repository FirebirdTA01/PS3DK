#!/usr/bin/env python3
"""rsx-cg-compiler --stats: one statistics line on stderr, output files unchanged.

For every fragment (.fcg) and vertex (.vcg) shader under tools/rsx-cg-compiler/tests/shaders that compiles:
  - the container written with --stats is byte-identical to the one written without it;
  - exactly one 'rsx-cg-compiler stats:' line is printed on stderr;
  - fragment: (instructions + inline_const_blocks) * 16 == ucode_bytes, uniform_patch_slots >= uniforms_embedded;
  - vertex: instructions * 16 == ucode_bytes.
Then generated shaders with 1, 255, 256 and 257 conditional discards report exactly that many KIL instructions.
Judged by exit status (0 = all checks hold, at least one shader of each profile compared)."""
import argparse, re, subprocess, sys, tempfile
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--rsxcg', required=True)
a = ap.parse_args()
root = Path(__file__).resolve().parents[2]
shaders = sorted((root / 'tools/rsx-cg-compiler/tests/shaders').glob('*.[fv]cg'))
seen = {'fp': 0, 'vp': 0}
with tempfile.TemporaryDirectory() as t:
    for s in shaders:
        prof = 'sce_fp_rsx' if s.suffix == '.fcg' else 'sce_vp_rsx'
        plain, stats = Path(t, 'a.bin'), Path(t, 'b.bin')
        r = subprocess.run([a.rsxcg, '-p', prof, '--emit-container', str(plain), str(s)], capture_output=True)
        if r.returncode != 0:
            continue   # a shader this compiler refuses is not this test's subject
        r = subprocess.run([a.rsxcg, '-p', prof, '--stats', '--emit-container', str(stats), str(s)],
                           capture_output=True, text=True)
        assert r.returncode == 0, (s.name, r.stderr)
        assert plain.read_bytes() == stats.read_bytes(), f'{s.name}: --stats changed the container'
        lines = [l for l in r.stderr.splitlines() if l.startswith('rsx-cg-compiler stats:')]
        assert len(lines) == 1, (s.name, r.stderr)
        f = dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', lines[0]))
        if prof == 'sce_fp_rsx':
            assert lines[0].startswith('rsx-cg-compiler stats: fp '), lines[0]
            assert (f['instructions'] + f['inline_const_blocks']) * 16 == f['ucode_bytes'], (s.name, f)
            assert f['uniform_patch_slots'] >= f['uniforms_embedded'], (s.name, f)
            seen['fp'] += 1
        else:
            assert lines[0].startswith('rsx-cg-compiler stats: vp '), lines[0]
            assert f['instructions'] * 16 == f['ucode_bytes'], (s.name, f)
            seen['vp'] += 1
assert seen['fp'] and seen['vp'], seen
# KIL is counted from the ucode, not from the container's 8-bit kill-count field: N conditional discards are
# N KIL instructions on both sides of the 8-bit boundary.
with tempfile.TemporaryDirectory() as t:
    for n in (1, 255, 256, 257):
        src = Path(t, f'kill{n}.fcg')
        body = ''.join(f'if(c.x < {i / 258:.9f}) discard;'+chr(10) for i in range(n))
        src.write_text('float4 main(float4 c:COLOR0):COLOR0 {'+chr(10)+body+'return c;'+chr(10)+'}'+chr(10))
        r = subprocess.run([a.rsxcg, '-p', 'sce_fp_rsx', '--stats', '--emit-container', str(Path(t, 'k.bin')), str(src)],
                           capture_output=True, text=True)
        assert r.returncode == 0, (n, r.stderr)
        line = [l for l in r.stderr.splitlines() if l.startswith('rsx-cg-compiler stats:')][0]
        kil = int(re.search('kil=([0-9]+)', line).group(1))
        assert kil == n, (n, line)
print(f"rsx-cg-compiler --stats: {seen['fp']} fragment and {seen['vp']} vertex programs, containers unchanged PASS")
