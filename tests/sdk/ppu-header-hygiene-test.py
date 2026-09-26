#!/usr/bin/env python3
"""Serial standalone probes of the merged installed PPU public header surface.

The prefix is read-only. Candidate SDK headers replace installed headers in
one private overlay, so include_next cannot accidentally find a second SDK.
Every probe keeps its command, complete diagnostics and input fingerprints.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import shutil
import signal
import subprocess
import time
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODES = [('gnu99', 'c', 'gcc'), ('c11', 'c', 'gcc'), ('c++17', 'c++', 'g++')]


def key(row):
    return row['header'], row['standard'], row['abi']


def residual_index(records):
    index = {}
    for row in records:
        if (set(row) != {'header', 'standard', 'abi', 'diagnostic'}
                or not all(isinstance(v, str) and v for v in row.values())
                or row['standard'] not in {m[0] for m in MODES}
                or row['abi'] not in {'ilp32', 'lp64'}):
            raise ValueError('invalid residual record: ' + repr(row))
        if key(row) in index:
            raise ValueError('duplicate residual key: ' + repr(key(row)))
        index[key(row)] = row['diagnostic']
    return index


def reconcile(rows, residuals, partial=False):
    """Known failures must persist exactly; improvements force list shrinkage."""
    errors = []
    seen = set()
    for row in rows:
        identity = key(row)
        if identity in seen:
            errors.append('duplicate result: ' + repr(identity))
        seen.add(identity)
        expected = residuals.get(identity)
        if expected is None:
            if row['status'] == 'NOT_APPLICABLE' and row.get('classification_reason'):
                continue
            if row['status'] != 'PASS':
                errors.append('unlisted failure: ' + repr(identity))
        elif row['status'] == 'PASS':
            errors.append('resolved residual must be removed: ' + repr(identity))
        elif row['status'] != 'FAIL' or row.get('rc') != 1 or row['diagnostic'] != expected:
            errors.append('residual changed: ' + repr(identity))
    if not partial:
        errors.extend('residual has no result: ' + repr(identity) for identity in sorted(set(residuals) - seen))
    return errors


def self_test():
    class RatchetTests(unittest.TestCase):
        def setUp(self):
            self.known = {'header': 'sys/example.h', 'standard': 'c11', 'abi': 'ilp32',
                          'diagnostic': "sys/example.h:3:1: error: unknown type name 'example_t'"}
            self.index = residual_index([self.known])
            self.fail = dict(self.known, status='FAIL', rc=1)

        def test_matching_failure(self):
            self.assertEqual(reconcile([self.fail], self.index), [])

        def test_unknown_failure(self):
            self.assertTrue(reconcile([self.fail], {}))

        def test_changed_diagnostic(self):
            self.assertTrue(reconcile([dict(self.fail, diagnostic='different error')], self.index))

        def test_resolved_failure_requires_shrink(self):
            self.assertTrue(reconcile([dict(self.fail, status='PASS', diagnostic='')], self.index))

        def test_removed_row_requires_shrink(self):
            self.assertTrue(reconcile([], self.index))

        def test_timeout_never_matches(self):
            self.assertTrue(reconcile([dict(self.fail, status='TIMEOUT')], self.index))

        def test_crash_with_matching_diagnostic_rejected(self):
            self.assertTrue(reconcile([dict(self.fail, rc=134)], self.index))

        def test_signal_with_matching_diagnostic_rejected(self):
            self.assertTrue(reconcile([dict(self.fail, rc=-11)], self.index))

        def test_unclassified_skip_rejected(self):
            self.assertTrue(reconcile([dict(self.fail, status='NOT_APPLICABLE')], {}))

        def test_duplicate_residual_rejected(self):
            with self.assertRaises(ValueError):
                residual_index([self.known, self.known])

        def test_invalid_schema_rejected(self):
            with self.assertRaises(ValueError):
                residual_index([dict(self.known, comment='not part of the public schema')])

        def test_duplicate_result_rejected(self):
            self.assertTrue(reconcile([self.fail, self.fail], self.index))

        def test_unlisted_pass(self):
            self.assertEqual(reconcile([dict(self.fail, status='PASS', diagnostic='')], {}), [])

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(RatchetTests))
    return int(not result.wasSuccessful())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_AS, (1024 ** 3, 1024 ** 3))
    os.nice(10)


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ps3dev', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--preload', type=Path, default=os.environ.get('PS3TC_NODUMP_PRELOAD'),
                        help='optional platform nodump library (or PS3TC_NODUMP_PRELOAD)')
    parser.add_argument('--timeout', type=float, default=20)
    parser.add_argument('--only', action='append', default=[], help='exact header; marks result as a partial run')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    prefix = args.ps3dev.resolve(strict=True)
    preload = args.preload.resolve(strict=True) if args.preload else None
    installed = prefix / 'ps3dk/ppu/include'
    compilers = {name: prefix / ('ppu/bin/powerpc64-ps3-elf-' + name) for name in ['gcc', 'g++']}
    for compiler in compilers.values():
        if not compiler.is_file() or not os.access(compiler, os.X_OK):
            parser.error('required compiler unavailable: ' + str(compiler))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    overlay = out / 'include'
    shutil.copytree(installed, overlay, symlinks=False)
    provenance = {'root': str(ROOT), 'prefix': str(prefix), 'preload': str(preload) if preload else None,
                  'preload_sha256': digest(preload) if preload else None, 'script_sha256': digest(Path(__file__)),
                  'limits': {'address_space': 1024 ** 3, 'core': 0, 'nice_increment': 10,
                             'timeout_seconds': args.timeout},
                  'installed': {}, 'candidate': {}, 'compilers': {}, 'dependencies': {}}
    for path in sorted(installed.rglob('*.h')):
        if path.is_file():
            provenance['installed'][path.relative_to(installed).as_posix()] = digest(path)
    for path in sorted((ROOT / 'sdk/include').rglob('*.h')):
        relative = path.relative_to(ROOT / 'sdk/include')
        target = overlay / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        provenance['candidate'][relative.as_posix()] = {'source': str(path), 'sha256': digest(path)}
    for relative in ['cell/gcm/gcm_cg_func.h', 'cell/gcm/ps3tc_fifo_wrap.h']:
        source = ROOT / 'sdk/libgcm_cmd/include' / relative
        shutil.copyfile(source, overlay / relative)
        provenance['candidate'][relative] = {'source': str(source), 'sha256': digest(source)}
    env = dict(os.environ, LC_ALL='C')
    if preload:
        env['LD_PRELOAD'] = str(preload)
    # Ambient include variables must not mask an omitted public dependency.
    for variable in ['CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'OBJC_INCLUDE_PATH']:
        env.pop(variable, None)
    for name, path in compilers.items():
        info = {'path': str(path), 'sha256': digest(path),
                'version': subprocess.check_output([str(path), '--version'], env=env, text=True).splitlines()[0]}
        internal = subprocess.check_output([str(path), '-print-prog-name=' + ('cc1plus' if name == 'g++' else 'cc1')], env=env, text=True).strip()
        info['internal'] = {'path': internal, 'sha256': digest(Path(internal))}
        provenance['compilers'][name] = info
    classification_path = ROOT / 'tests/sdk/ppu-header-hygiene-classification.json'
    classifications = json.loads(classification_path.read_text())
    provenance['classification_sha256'] = digest(classification_path)
    residual_path = ROOT / 'tests/sdk/ppu-header-hygiene-residuals.json'
    residuals = residual_index(json.loads(residual_path.read_text()))
    provenance['residuals_sha256'] = digest(residual_path)
    headers = sorted(path.relative_to(overlay).as_posix() for path in overlay.rglob('*.h') if path.is_file())
    unknown = set(classifications) - set(headers)
    if unknown:
        raise RuntimeError('classification refers to missing headers: ' + repr(sorted(unknown)))
    if args.only:
        unknown = set(args.only) - set(headers)
        if unknown:
            raise RuntimeError('requested headers missing: ' + repr(sorted(unknown)))
        headers = sorted(set(args.only))
    provenance['partial'] = bool(args.only)
    provenance['headers'] = headers
    provenance['overlay'] = {p.relative_to(overlay).as_posix(): digest(p) for p in sorted(overlay.rglob('*')) if p.is_file()}
    (out / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    probes = out / 'probes'
    probes.mkdir()
    rows = []
    started = time.monotonic()
    print(f'Inventory: {len(provenance["installed"])} installed headers; {len(headers)} merged headers to probe', flush=True)
    for header in headers:
        classification = classifications.get(header)
        if classification:
            # Exceptions are explicit prerequisites, never expected failures.
            if (set(classification) != {'owner', 'reason', 'standards'} or not classification['reason']
                    or not classification['standards']
                    or len(set(classification['standards'])) != len(classification['standards'])
                    or set(classification['standards']) - {m[0] for m in MODES}):
                raise RuntimeError('invalid prerequisite classification: ' + header)
            owner = classification['owner']
            if owner not in provenance['overlay']:
                raise RuntimeError('missing prerequisite owner: ' + owner)
        else:
            owner = header
        for standard, language, compiler in MODES:
            for abi, width in [('ilp32', 4), ('lp64', 8)]:
                ident = f'{len(rows):04d}'
                if classification and standard not in classification['standards']:
                    row = {'header': header, 'standard': standard, 'abi': abi, 'status': 'NOT_APPLICABLE',
                           'diagnostic': '', 'rc': None, 'owner': owner,
                           'classification_reason': classification['reason'], 'command': []}
                    rows.append(row)
                    with (out / 'rows.jsonl').open('a') as log:
                        log.write(json.dumps(row) + '\n')
                    continue
                source = probes / (ident + '.c')
                assertion = 'static_assert' if language == 'c++' else '_Static_assert'
                source.write_text(f'#include <{owner}>\n#include <{owner}>\n'
                                  f'{assertion}(sizeof(void *) == {width}, "selected PPU ABI");\n')
                depfile = probes / (ident + '.d')
                command = [str(compilers[compiler]), '-x', language, '-std=' + standard,
                           '-mcpu=cell', '-mhard-float', '-Wall', '-Wextra', '-Werror',
                           '-I', str(overlay), '-MD', '-MF', str(depfile), '-fsyntax-only', str(source)]
                if abi == 'lp64':
                    command.insert(1, '-mlp64')
                before = time.monotonic()
                proc = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, start_new_session=True, preexec_fn=limits)
                timed_out = False
                try:
                    output, _ = proc.communicate(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    os.killpg(proc.pid, signal.SIGKILL)
                    output, _ = proc.communicate()
                status = 'TIMEOUT' if timed_out else {0: 'PASS', 1: 'FAIL'}.get(proc.returncode, 'ERROR')
                dependencies = set()
                if depfile.is_file():
                    dependencies = set(shlex.split(depfile.read_text().replace('\\\n', '').split(':', 1)[1]))
                for name in dependencies:
                    if name not in provenance['dependencies']:
                        provenance['dependencies'][name] = digest(Path(name))
                if status == 'PASS' and str(overlay / header) not in dependencies:
                    status = 'ERROR'
                    output += '\nerror: probed header was not read through its owning header\n'
                (probes / (ident + '.log')).write_text(output)
                diagnostic = next((line for line in output.splitlines() if 'error:' in line), '')
                diagnostic = diagnostic.replace(str(overlay) + '/', '').replace(str(prefix) + '/', '<toolchain>/')
                row = {'header': header, 'standard': standard, 'abi': abi, 'status': status, 'diagnostic': diagnostic,
                       'owner': owner, 'rc': proc.returncode, 'seconds': round(time.monotonic() - before, 3),
                       'command': command, 'diagnostics': str(probes / (ident + '.log'))}
                rows.append(row)
                with (out / 'rows.jsonl').open('a') as log:
                    log.write(json.dumps(row) + '\n')
        if len(rows) % 120 == 0:
            print(f'{len(rows)}/{len(headers)*6}: {dict(collections.Counter(r["status"] for r in rows))}', flush=True)
    errors = reconcile(rows, residuals, partial=bool(args.only))
    (out / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    summary = {'partial': bool(args.only), 'headers': len(headers), 'rows': len(rows),
               'counts': dict(collections.Counter(row['status'] for row in rows)),
               'ratchet_errors': errors,
               'failed_headers': sorted({r['header'] for r in rows if r['status'] != 'PASS'}),
               'seconds': round(time.monotonic() - started, 3)}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2), flush=True)
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
