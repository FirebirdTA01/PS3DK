#!/usr/bin/env python3
"""Run the real artifact checker on fixtures and the repository's sample inputs."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / 'scripts/gen-required-artifacts.sh'
MANIFEST = ROOT / 'cmake/ps3-required-artifacts.txt'
ROWS = 'ppu_stub ppu/lib/libfoo.a\nppu_stub ppu/lib/libbar.a\nspu_lib spu/lib/libbaz.a\n'
LINKS = '''target_link_libraries(probe PRIVATE foo "bar" "${PRIVATE_ARCHIVE}" ${OTHER_ARCHIVE})
ps3_add_spu_image(probe_spu NAME image LIBS baz)
'''
STUB = 'ps3_prx_stub_library(local_prx_stub EXPORTS exports.yml)\n'


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')


def tree(path, samples, manifest=ROWS, archives=None):
    write(path / 'scripts/gen-required-artifacts.sh', GENERATOR.read_text())
    write(path / 'cmake/ps3-required-artifacts.txt', manifest)
    for name, text in samples.items():
        write(path / 'samples' / name / 'CMakeLists.txt', text)
    for subdir in ('ppu/lib', 'spu/lib'):
        (path / 'install' / subdir).mkdir(parents=True, exist_ok=True)
    for archive in archives if archives is not None else (
            'ppu/lib/libfoo.a', 'ppu/lib/libbar.a', 'spu/lib/libbaz.a'):
        write(path / 'install' / archive, '')


def check(bash, path, expected, message, env, mode='--check'):
    command = [bash, (path / 'scripts/gen-required-artifacts.sh').as_posix(),
               (path / 'install').as_posix(), mode]
    write(path / ('command-' + mode.lstrip('-') + '.txt'), repr(command) + '\n')
    logfile = path / ('run-' + mode.lstrip('-') + '.log')
    # Files, rather than PIPE, avoid waiting indefinitely on inherited pipe
    # handles when a Windows shell descendant survives a timeout.
    with logfile.open('wb') as log:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, env=env,
                                   start_new_session=(os.name != 'nt'))
        try:
            result = process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            if os.name == 'nt':
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], check=True)
            else:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise AssertionError(f'{path.name}: timed out; see {logfile}')
    output = logfile.read_text(encoding='utf-8', errors='replace')
    if result != expected or message not in output:
        raise AssertionError(f'{path.name}: rc={result}, expected {expected} and {message!r}\n{output}')
    print(f'PASS {path.name} {mode} rc={result}: {message}', flush=True)
    return output


def run(bash, work):
    env = dict(os.environ)
    # Native Windows Python needs the Git Bash utilities ahead of Windows
    # find/sort; --bash must name Git's usr/bin/bash.exe on that host.
    env['PATH'] = str(Path(bash).parent) + os.pathsep + env.get('PATH', '')
    passed = failed = 0

    def case(name, samples, expected=0, message='manifest covers all 3',
             manifest=ROWS, archives=None, split=False):
        nonlocal passed, failed
        path = work / name
        tree(path, samples, manifest, archives)
        local_env = dict(env)
        if split:
            # Deterministically reproduce xargs batches where one input has
            # no generated-stub declaration. Do not hide any child status.
            xargs = shutil.which('xargs', path=env['PATH'])
            if not xargs:
                raise RuntimeError('xargs unavailable')
            wrapper = path / 'bin/xargs'
            write(wrapper, '#!/usr/bin/env bash\nexec ' + shlex.quote(Path(xargs).as_posix()) + ' -n 1 "$@"\n')
            wrapper.chmod(0o755)
            local_env['PATH'] = str(wrapper.parent) + os.pathsep + env['PATH']
        try:
            check(bash, path, expected, message, local_env)
            passed += 1
        except AssertionError as error:
            print(f'FAIL {error}', flush=True)
            failed += 1

    case('quoted-variables-and-literals', {'one': LINKS + STUB})
    case('no-generated-stubs', {'one': LINKS})
    case('split-no-match-batch', {'one': LINKS, 'two': STUB}, split=True)
    case('generated-stub-excluded', {'one': LINKS + STUB +
         'target_link_libraries(probe PRIVATE local_prx_stub)\n'})
    case('missing-literal', {'one': LINKS + STUB}, 1, 'ppu/lib/libbar.a',
         archives=['ppu/lib/libfoo.a', 'spu/lib/libbaz.a'])
    case('uncovered-literal', {'one': LINKS + STUB}, 1, 'MANIFEST INCOMPLETE',
         manifest=ROWS.replace('ppu_stub ppu/lib/libbar.a\n', ''))
    case('zero-ppu-floor', {'one': LINKS + STUB}, 1, 'PPU scanner produced 0 rows',
         archives=['spu/lib/libbaz.a'])
    case('zero-spu-floor', {'one': LINKS + STUB}, 1, 'SPU scanner produced 0 rows',
         archives=['ppu/lib/libfoo.a', 'ppu/lib/libbar.a'])

    # The repo check is seeded from committed promises, NOT scanner output.
    # Empty files test coverage only: the real packager validates actual bytes.
    repo = work / 'repository-samples'
    samples = {p.parent.relative_to(ROOT / 'samples').as_posix(): p.read_text()
               for p in (ROOT / 'samples').rglob('CMakeLists.txt')}
    manifest = MANIFEST.read_text()
    archives = []
    for line in manifest.splitlines():
        words = line.split()
        if len(words) >= 2 and not line.lstrip().startswith('#') and words[1].endswith('.a'):
            item = Path(words[1])
            if item.is_absolute() or '..' in item.parts:
                raise AssertionError('unsafe manifest path: ' + words[1])
            archives.append(words[1])
    # libm comes from newlib, outside the SDK manifest. Keep it outside
    # ppu/lib so it satisfies lookup without inventing a manifest row.
    archives.append('ppu/powerpc64-ps3-elf/lib/libm.a')
    tree(repo, samples, manifest, archives)
    try:
        check(bash, repo, 0, 'manifest covers all ', env)
        passed += 1
    except AssertionError as error:
        print(f'FAIL {error}', flush=True)
        failed += 1
    print(f'required-artifacts: {passed} passed, {failed} failed', flush=True)
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bash', default=shutil.which('bash'))
    parser.add_argument('--output', type=Path, help='retain a new fixture/evidence directory')
    args = parser.parse_args()
    if not args.bash:
        parser.error('bash is required')
    bash = str(Path(args.bash).resolve())
    if args.output:
        work = args.output.resolve()
        work.mkdir(parents=True, exist_ok=False)
        return run(bash, work)
    with tempfile.TemporaryDirectory(prefix='required artifacts ') as directory:
        return run(bash, Path(directory))


if __name__ == '__main__':
    raise SystemExit(main())
