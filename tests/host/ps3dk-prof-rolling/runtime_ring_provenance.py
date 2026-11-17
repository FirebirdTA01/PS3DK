#!/usr/bin/env python3
"""Verify a fresh real-runtime receipt; no build or producer launch.

This records evidence provenance, not authentication or hardware qualification.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--runtime-dir', type=Path, required=True)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    repo, runtime, source = a.repo.resolve(), a.runtime_dir.resolve(), a.source.resolve()
    assert source.stat().st_size <= 4 * 1024 * 1024
    commands = json.loads((runtime / 'commands.json').read_text())
    assert commands and all(c['rc'] == 0 for c in commands)
    assert sum(c['name'] == 'run' for c in commands) == 1
    assert sum(c['name'] == 'reader' for c in commands) == 1
    assert b'AUTO ring fixture PASS\n' in (runtime / 'run.stdout').read_bytes()
    originals = list((runtime / 'capture').glob('*.prof'))
    assert len(originals) == 1 and originals[0].read_bytes() == source.read_bytes()
    checker = repo / 'tests/host/ps3dk-prof-runtime/auto_ring_capture.py'
    spec = importlib.util.spec_from_file_location('runtime_ring_wire_check', checker)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    summary = module.verify(source.read_bytes(), pending_tail=0)
    assert summary['held'] == 0 and summary['rejected'] == (0, 0, 0)
    assert summary['frames_over_504'] >= 1
    assert summary['blocks_retired'] == summary['blocks_issued']
    assert summary['status_flags'] & 1 and not summary['status_flags'] & 2
    # Source implementation hashes are distinct from the runtime runner's actual
    # compile/link commands. Both belong in the receipt; neither replaces the other.
    files = [checker, repo / 'tests/host/ps3dk-prof-runtime/auto-ring-runtime-test.c',
             repo / 'tests/host/ps3dk-prof-runtime/auto-runtime-test.py',
             runtime / 'commands.json', runtime / 'run.stdout', runtime / 'reader.stdout']
    receipt = dict(provenance='real host runtime AUTO ring mode 3; independent command executor; no real RSX',
                   source_sha256=sha(source), source_bytes=source.stat().st_size,
                   modeled_mixed_source=False, producer_commands=commands,
                   evidence_hashes={str(f): sha(f) for f in files}, wire_summary=summary)
    with a.out.open('x', encoding='utf-8') as f:
        json.dump(receipt, f, indent=2)
        f.write('\n')
    print('PASS real runtime source provenance and ring wire truth; encoder not invoked')


if __name__ == '__main__':
    main()
