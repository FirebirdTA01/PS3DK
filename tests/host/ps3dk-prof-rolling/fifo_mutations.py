#!/usr/bin/env python3
"""FIFO restart sensitivity controls in an isolated Rust workspace copy.

Caller owns build authority. Never edits the checkout. A fresh output directory
is required; baseline must pass, and compiler failures do not count as kills.
--check-sites only reads source and performs no builds or filesystem mutations.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
TOOLS = REPO / 'tools'
SOURCE = TOOLS / 'ps3dk-prof/src/rolling_fifo.rs'
RETURN = '    Ok(state)\n}'
CASES = [
    ('forgets_last_sequence', 'suffix_status_still_uses_prefix_last_written_sequence_and_counts',
     [(RETURN, '    state.reader.last=None;\n'+RETURN)]),
    ('forgets_valid_clock', 'saved_phase_frozen_fields_and_valid_clock_fence_remain_strict',
     [(RETURN, '    state.reader.last_valid_tb=None;\n'+RETURN)]),
    ('clears_saturation', 'saturation_and_final_fences_cannot_be_reset_by_a_cut',
     [(RETURN, '    state.reader.sat_seen=false;\n'+RETURN)]),
    ('skips_window_close', 'accounting43_cursor_and_later_window_close_survive_eviction',
     [('self.reader.result.accounting.window_closed(window).map_err(Error::Malformed)?;',
       'let _ = self.reader.result.accounting.window_closed(window);')]),
    ('resets_outer_work', 'actual_suffix_and_outer_chain_share_work_without_resetting_allowance',
     [('if n > self.left || n > self.outer.remaining()', 'if n > self.left'),
      ('self.outer.used += n;', 'self.outer.used = self.outer.used.saturating_add(n).min(self.outer.limit);')]),
    ('charges_evicted_history', 'huge_evicted_census_import_cost_is_typed_state_size_not_history_length',
     [('state.historical_work=u64_at(b,32);',
       'state.historical_work=u64_at(b,32); if state.historical_work>limits.work_records {return Err(Error::Limit("mutant lifetime work"));}')]),
    ('forgets_boundary_membership', 'crossing_outer_and_phase_are_censored_but_later_inner_phase_survives',
     [('let boundary_frames=state.runs.clone();', 'let boundary_frames=Vec::new();')]),
    ('measures_prefix_endpoint', 'crossing_outer_and_phase_are_censored_but_later_inner_phase_survives',
     [('    state.apply(refs,suffix,next_chunk,limits,&mut meter,true)?;',
       '    if let Some(e)=state.reader.last.clone() {state.reader.result.events.push(e);}\n    state.apply(refs,suffix,next_chunk,limits,&mut meter,true)?;')]),
]

def variants(source, selected):
    for name, witness, changes in CASES:
        if selected and name not in selected:
            continue
        text = source
        for before, after in changes:
            if text.count(before) != 1:
                raise RuntimeError(f'{name}: expected exactly one mutation site for {before!r}')
            text = text.replace(before, after)
        yield name, witness, text

def copy_workspace(destination):
    destination.mkdir(parents=True, exist_ok=False)
    manifest = (TOOLS / 'Cargo.toml').read_text(encoding='utf-8')
    manifest, changed = re.subn(r'members\s*=\s*\[.*?\]', 'members = ["ps3dk-prof"]', manifest, count=1, flags=re.S)
    if changed != 1:
        raise RuntimeError('workspace members declaration not found')
    (destination / 'Cargo.toml').write_text(manifest, encoding='utf-8')
    for name in ['Cargo.lock', 'VERSION']:
        shutil.copy2(TOOLS / name, destination / name)
    shutil.copytree(TOOLS / 'ps3dk-version', destination / 'ps3dk-version')
    crate = destination / 'ps3dk-prof'
    crate.mkdir()
    shutil.copy2(TOOLS / 'ps3dk-prof/Cargo.toml', crate / 'Cargo.toml')
    shutil.copytree(TOOLS / 'ps3dk-prof/src', crate / 'src')
    (crate / 'tests').mkdir()
    for name in ['common', 'fifo_support']:
        shutil.copytree(TOOLS / 'ps3dk-prof/tests' / name, crate / 'tests' / name)
    shutil.copy2(TOOLS / 'ps3dk-prof/tests/rolling_fifo.rs', crate / 'tests/rolling_fifo.rs')
    return crate / 'src/rolling_fifo.rs'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cargo', default='cargo')
    parser.add_argument('--case', action='append', choices=[c[0] for c in CASES])
    parser.add_argument('--check-sites', action='store_true')
    args = parser.parse_args()
    source = SOURCE.read_text(encoding='utf-8')
    selected = list(variants(source, args.case))
    if args.check_sites:
        print(f'PASS {len(selected)} FIFO mutation sites; no compiler or execution')
        return
    out = args.build_dir.resolve()
    if out == REPO or REPO in out.parents:
        raise RuntimeError('build directory must be outside the source checkout')
    isolated = out / 'isolated'
    altered = copy_workspace(isolated)
    original_digest = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
    (out / 'source.sha256').write_text(original_digest+'\n', encoding='utf-8')
    env = dict(os.environ, CARGO_TARGET_DIR=str(out / 'target'))
    commands = [('baseline', None, source)] + selected
    try:
        for name, witness, text in commands:
            altered.write_text(text, encoding='utf-8')
            command = [args.cargo, 'test', '--offline', '--manifest-path', str(isolated / 'Cargo.toml'),
                       '-p', 'ps3dk-prof', '--test', 'rolling_fifo', '--jobs', '4']
            if witness:
                command += [witness, '--', '--exact']
            result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=180)
            log = result.stdout + result.stderr
            (out / f'{name}.log').write_text(log, encoding='utf-8')
            if name == 'baseline':
                if result.returncode:
                    raise RuntimeError(f'baseline failed; see {out / "baseline.log"}')
                print('PASS FIFO baseline', flush=True)
            elif result.returncode != 101 or f'test {witness} ... FAILED' not in log or 'could not compile' in log:
                raise RuntimeError(f'{name}: survived or non-test failure; see {out / (name+".log")}')
            else:
                print(f'KILLED {name}: {witness}', flush=True)
    finally:
        altered.write_text(source, encoding='utf-8')
        if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != original_digest:
            raise RuntimeError('original source changed during run; fixture not coherent')
    print(f'PASS {len(selected)} isolated FIFO restart sensitivity controls')

if __name__ == '__main__':
    main()
