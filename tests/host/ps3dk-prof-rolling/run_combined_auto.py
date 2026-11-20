#!/usr/bin/env python3
"""Run prebuilt AUTO source controls before attributable encoder RED/GREEN.

No compilation or producer generation. Each test has an explicit wall timeout;
on POSIX its whole process group is killed on timeout. A timeout never counts as
the expected refusal. Input hashes and per-test output are retained on failure.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--test', type=Path, required=True)
    p.add_argument('--encoder', type=Path, required=True)
    p.add_argument('--runtime-source', type=Path, required=True)
    p.add_argument('--provenance', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=300)
    p.add_argument('--expect-red', action='store_true')
    a = p.parse_args()
    if not 1 <= a.timeout <= 900:
        p.error('--timeout must be 1..900 seconds per test')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    test, encoder, source = a.test.resolve(), a.encoder.resolve(), a.runtime_source.resolve()
    proof = json.loads(a.provenance.read_text())
    if source.stat().st_size > 4 << 20 or proof['source_sha256'] != sha(source):
        raise RuntimeError('runtime source does not match bounded B0 provenance')
    inputs = [test, encoder, source, a.provenance.resolve(), Path(__file__).resolve()]
    before = {str(f): sha(f) for f in inputs}
    env = dict(os.environ, PS3DK_PROF_ROLLING_AUTO_RUNTIME_SOURCE=str(source),
               PS3DK_PROF_ROLLING_COMBINED_AUTO_ENCODER=str(encoder))
    cases = [
        ('modeled_legacy7_and_ring2047_mixed_sources_validate_before_encoder', False, False),
        ('real_runtime_ring_original_and_every_prefix_validate_before_encoder', True, False),
        ('c_auto_legacy7_ppu_spu_fifo_matches_every_original_cut', True, True),
        ('c_auto_ring2047_ppu_spu_fifo_matches_every_original_cut', True, True),
        ('c_auto_real_runtime_ring_matches_every_original_cut', True, True),
    ]
    results = []
    for name, ignored, semantic in cases:
        command = [str(test), '--exact', name, '--nocapture'] + (['--ignored'] if ignored else [])
        started = time.monotonic()
        timed_out = False
        with (out / (name + '.stdout')).open('wb') as stdout, (out / (name + '.stderr')).open('wb') as stderr:
            child = subprocess.Popen(command, env=env, stdout=stdout, stderr=stderr,
                                     start_new_session=os.name == 'posix')
            try:
                rc = child.wait(timeout=a.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                if os.name == 'posix':
                    os.killpg(child.pid, signal.SIGKILL)
                else:
                    child.kill()
                rc = child.wait(timeout=10)
        stdout = (out / (name + '.stdout')).read_bytes()
        stderr = (out / (name + '.stderr')).read_bytes()
        want_red = semantic and a.expect_red
        if want_red:
            accepted = (not timed_out and rc == 101 and b'0 passed; 1 failed' in stdout
                        and b'canonical prefixes BEFORE C' in stderr
                        and b'C AUTO schema2 encoder rejected valid original' in stderr
                        and b'reject stage=auto-adapter rc=-100' in stderr)
        else:
            accepted = not timed_out and rc == 0 and b'1 passed; 0 failed' in stdout
        results.append(dict(name=name, command=command, seconds=time.monotonic()-started,
                            timeout_seconds=a.timeout, timed_out=timed_out, actual_rc=rc,
                            expected='named scaffold RED' if want_red else 'GREEN', accepted=accepted))
        receipt = dict(inputs=before, inputs_unchanged=all(sha(f) == before[str(f)] for f in inputs),
                       source_provenance=proof, results=results)
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(f'{name}: rc={rc} timed_out={timed_out} accepted={accepted}', flush=True)
        if not accepted or not receipt['inputs_unchanged']:
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
