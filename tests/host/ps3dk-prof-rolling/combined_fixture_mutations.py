#!/usr/bin/env python3
"""Independent C semantic encoder sensitivity, without checkout edits.

Parent supplies build authority and a prebuilt rolling_combined_c test binary.
Each altered C source is compiled outside the checkout; original source.prof
must remain byte-identical, all checkpoint CRCs valid, and the ordinary-reader
gate must pass before the direct-prefix equality assertion kills a mutant.
Compiler/producer failures and the later physical join are not mutation kills.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import zlib

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
SOURCE = HERE / 'combined_fixture.c'
IMPL = REPO / 'sdk/libps3dkprof/src/rolling'
TEST = 'c_accepted_output_mixed_checkpoints_and_store_match_original_source'
CASES = [
    ('next_original_chunk', 'u64(out, s->next_chunk);', 'u64(out, s->next_chunk + 1);'),
    ('prefix_byte_frontier', 'u64(out, s->prefix_bytes);', 'u64(out, s->prefix_bytes + 20);'),
    ('records_count_as_rows', 'u64(&body, l->record_chunks);', 'u64(&body, l->written);'),
    ('support_ordinal', 'u32(&body, j->ordinal);', 'u32(&body, j->ordinal + 1);'),
    ('active_parent_record', 'raw(&body, st->parent.bytes, 32);', 'raw(&body, st->begin.bytes, 32);'),
    ('active_frame_context', 'u32(&body, have_frame ? 1 : 0);', 'u32(&body, 0);'),
    ('spu_begin_raw_witness', 'u32(&body, e->raw);', 'u32(&body, e->raw + 1);'),
    ('spu_chunk_census', 'u64(&body, j->chunks);', 'u64(&body, j->events);'),
    ('spu_block_last_original',
     'u32(&body, 96 + 24 * j->depth); u64(&body, j->first); u64(&body, j->last);',
     'u32(&body, 96 + 24 * j->depth); u64(&body, j->first); u64(&body, j->first);'),
]


def variants(original):
    for name, before, after in CASES:
        if original.count(before) != 1:
            raise RuntimeError(f'{name}: expected exactly one source mutation site: {before!r}')
        yield name, original.replace(before, after)


def run(command, path, env=None):
    result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=180)
    log = result.stdout + result.stderr
    path.write_text(log, encoding='utf-8')
    return result.returncode, log


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path)
    p.add_argument('--parity-exe', type=Path)
    p.add_argument('--cc', default='cc')
    p.add_argument('--check-sites', action='store_true')
    args = p.parse_args()
    original = SOURCE.read_text(encoding='utf-8')
    cases = list(variants(original))
    if args.check_sites:
        print(f'PASS {len(cases)} C semantic encoder mutation sites; no compiler or execution')
        return
    if not args.build_dir or not args.parity_exe:
        p.error('--build-dir and --parity-exe are required for execution')
    out = args.build_dir.resolve()
    if out == REPO or REPO in out.parents:
        p.error('build directory must be outside the source checkout')
    parity = args.parity_exe.resolve(strict=True)
    if not parity.is_file():
        p.error('parity executable must be an existing prebuilt Rust test binary')
    out.mkdir(parents=True, exist_ok=False)
    source_hash = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
    (out / 'source.sha256').write_text(source_hash + '\n', encoding='utf-8')
    baseline = None
    try:
        for name, altered in [('baseline', original)] + cases:
            case = out / name
            case.mkdir()
            c_path = case / 'combined_fixture.c'
            c_path.write_text(altered, encoding='utf-8')
            exe = case / ('fixture.exe' if os.name == 'nt' else 'fixture')
            command = shlex.split(args.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-I', str(IMPL), str(c_path), str(IMPL / 'roll_store.c'), '-o', str(exe)]
            rc, _ = run(command, case / 'compile.log')
            if rc:
                raise RuntimeError(f'{name}: compilation failed; not a sensitivity kill')
            artifacts = case / 'c-combined'
            artifacts.mkdir()
            rc, _ = run([str(exe), str(artifacts)], case / 'producer.log')
            if rc:
                raise RuntimeError(f'{name}: C producer failed; not a semantic parity kill')
            source = (artifacts / 'source.prof').read_bytes()
            if baseline is None:
                baseline = source
            elif source != baseline:
                raise RuntimeError(f'{name}: mutation changed the original source, not just its checkpoint')
            for i in range(13):
                cp = (artifacts / f'cut-{i:03}.ckp').read_bytes()
                if len(cp) < 84 or cp[:8] != b'PFRMIX01' or zlib.crc32(cp[:-4]) != int.from_bytes(cp[-4:], 'little'):
                    raise RuntimeError(f'{name}: checkpoint {i} has invalid framing/CRC; not a semantic kill')
            env = dict(os.environ, PS3DK_PROF_ROLLING_COMBINED_FIXTURE_DIR=str(artifacts))
            rc, log = run([str(parity), TEST, '--ignored', '--exact', '--nocapture'], case / 'parity.log', env)
            if name == 'baseline':
                if rc:
                    raise RuntimeError(f'baseline failed; see {case / "parity.log"}')
                print('PASS C semantic encoder baseline', flush=True)
            elif (rc != 101 or f'test {TEST} ... FAILED' not in log
                    or 'PASS ordinary C source: complete PPU12/SPU6, three spans, exact causal endpoints' not in log
                    or 'C accepted-output checkpoint serializer must match direct original prefix at cut' not in log):
                raise RuntimeError(f'{name}: survived or did not fail the checkpoint parity assertion; see {case / "parity.log"}')
            else:
                print(f'KILLED {name}: valid-CRC checkpoint differs from its accepted original prefix', flush=True)
    finally:
        if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != source_hash:
            raise RuntimeError('original C source changed during sensitivity run')
    print(f'PASS {len(cases)} isolated independent C encoder controls')


if __name__ == '__main__':
    main()
