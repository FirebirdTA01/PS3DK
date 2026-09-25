#!/usr/bin/env python3
"""SPU shared links must fail explicitly instead of producing executables.

Removing the driver's shared-mode refusal must fail the direct-link rows,
even if the ordinary CRT happens to cause a different linker error. PIC
compilation remains supported; its static-initializer relocation diagnostic
must remain intact. --trace-only performs no compilation or linking.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    if os.name == 'posix':
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (1024 ** 3, 1024 ** 3))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('SPU_CC'))
    parser.add_argument('--cxx', default=os.environ.get('SPU_CXX'))
    parser.add_argument('--trace-only', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('build/spu-shared-refusal'))
    args = parser.parse_args()
    if not args.cc and os.environ.get('PS3DEV'):
        args.cc = str(Path(os.environ['PS3DEV']) / 'spu/bin/spu-elf-gcc')
    if not args.cc:
        print('SPU shared refusal: SKIP (set SPU_CC or --cc)')
        return 0
    if not args.cxx:
        args.cxx = re.sub(r'gcc(?=(?:\.exe)?$)', 'g++', args.cc)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'results.json').exists():
        parser.error('choose a fresh output directory; evidence is not overwritten')
    source = out / 'ordinary.c'
    source.write_text('int main(void) { return 0; }\n')
    rows = []

    def run(name, driver, flags, source_path, suffix, expected, diagnostic=None):
        target = out / (name + suffix)
        if target.exists():
            parser.error('output already exists: ' + str(target))
        command = [driver, *flags, str(source_path), '-o', str(target)]
        try:
            proc = subprocess.run(command, capture_output=True, text=True, timeout=30)
            rc, text = proc.returncode, proc.stdout + proc.stderr
        except subprocess.TimeoutExpired as error:
            rc, text = 124, str(error)
        (out / (name + '.log')).write_text(text)
        ok = rc == expected
        if diagnostic:
            ok &= diagnostic in text and not target.exists()
        elif '-###' in flags:
            ok &= not target.exists()
        else:
            ok &= target.is_file() and target.stat().st_size > 0
        row = dict(name=name, command=command, rc=rc, output_exists=target.exists(), ok=ok)
        rows.append(row)
        print(name, 'rc=' + str(rc), flush=True)
        return row, text

    suppression = [[], ['-nostartfiles'], ['-nodefaultlibs'], ['-nostdlib'],
                   ['-r'], ['-static'], ['-nostdlib', '-nostartfiles', '-nodefaultlibs']]
    for language, driver in [('c', args.cc), ('cxx', args.cxx)]:
        for index, extra in enumerate(suppression):
            row, text = run(language + '-shared-trace-' + str(index), driver,
                            ['-###', '-shared', *extra], source, '.elf', 1,
                            'SPUDLL shared links are not supported')
            row['ok'] &= 'collect2' not in text
        for mode in ['', '-mspurs-task', '-mspurs-job', '-mspurs-job-initialize']:
            label = mode.removeprefix('-mspurs-') or 'ordinary'
            row, text = run(language + '-' + label + '-trace', driver,
                            ['-###', *([mode] if mode else [])], source, '.elf', 0)
            row['ok'] &= 'collect2' in text and 'SPUDLL shared links are not supported' not in text
        for action in ['-c', '-S', '-E']:
            row, text = run(language + '-shared-' + action[1:] + '-trace', driver,
                            ['-###', '-shared', '-fpic', action], source, '.out', 0)
            row['ok'] &= 'collect2' not in text

    if not args.trace_only:
        obj = out / 'ordinary.o'
        row, _ = run('ordinary', args.cc, ['-c'], source, '.o', 0)
        if row['ok']:
            for language, driver in [('c', args.cc), ('cxx', args.cxx)]:
                for index, extra in enumerate(suppression):
                    run(language + '-shared-link-' + str(index), driver,
                        ['-shared', *extra], obj, '.elf', 1,
                        'SPUDLL shared links are not supported')
                for index, flags in enumerate([
                        ['-Wl,-shared'], ['-Wl,--shared'], ['-Xlinker', '-shared'],
                        ['-nostdlib', '-Wl,-shared'], ['-nostartfiles', '-Xlinker', '-shared']]):
                    run(language + '-forwarded-shared-' + str(index), driver,
                        flags, obj, '.elf', 1, '-shared not supported')
                run(language + '-ordinary-link', driver, [], obj, '.elf', 0)
        for language, driver in [('c', args.cc), ('cxx', args.cxx)]:
            for action, suffix in [('-c', '.o'), ('-S', '.s'), ('-E', '.i')]:
                run(language + '-pic-shared-' + action[1:], driver,
                    ['-shared', '-fpic', action], source, suffix, 0)
        initializer = out / 'initializer.cpp'
        initializer.write_text('extern void initialize();\n'
                               'struct Init { Init() { initialize(); } };\n'
                               'Init instance;\n')
        run('initializer-nonpic', args.cxx, ['-std=c++98', '-c'], initializer, '.o', 0)
        for name, flags in [('pic', ['-fpic']), ('pic-shared', ['-fpic', '-shared'])]:
            run('initializer-' + name, args.cxx, ['-std=c++98', '-c', *flags],
                initializer, '.o', 1, 'creating run-time relocation')
    (out / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
    for row in rows:
        if not row['ok']:
            print('FAIL:', row['name'])
    passed = sum(row['ok'] for row in rows)
    print(f'SPU shared refusal: {passed}/{len(rows)} PASS' +
          (' (trace only; no compile/link claims)' if args.trace_only else ''))
    return 0 if passed == len(rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
