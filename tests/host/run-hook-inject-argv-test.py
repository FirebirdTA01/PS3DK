#!/usr/bin/env python3
"""The injected main hands the sample the arguments the host passed.

tests/hardware/psgl-sample-boot/run-hook-inject.cmake renames a sample's main
to ps3dk_sample_main and adds hook_main.c, which starts the ps3dk-run hook and
then calls the sample.  It used to call ps3dk_sample_main() with no arguments,
so on a console every hooked sample saw argc 0 and no argv: the arguments given
after "--" to ps3dk-run never arrived.

Builds five host samples through the real injection (C and C++, each with a
main taking argc/argv and one taking none, plus a C++ main(int, const char *[])) against a stand-in hook that, like
the real one, removes its --ps3dk-run= marker from argv.  Runs each with two
user arguments and the marker, and checks what main saw: the program name,
the two arguments, and no marker.  The no-argument mains must still build and
run.  Host only: no PS3 toolchain."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

MARKER = '--ps3dk-run=7:127.0.0.1:4300:0'

HOOK_C = r'''#include <string.h>
#include "ps3dk_run_hook.h"
/* Stand-in for the real hook's argv handling: find the marker, remove it.  It carries the real hook's two
   strings, so tools/ps3dk-run/hook-check.py can be checked against these builds. */
const char ps3dk_run_hook_strings[] = "ps3dk-run-watchdog";
int ps3dk_run_hook_start(int *argc, const char ***argv)
{
    (void)ps3dk_run_hook_strings;
    for (int i = 0; i < *argc; i++)
        if ((*argv)[i] && strncmp((*argv)[i], "--ps3dk-run=", 12) == 0) {
            for (int j = i; j < *argc; j++) (*argv)[j] = (*argv)[j + 1];
            (*argc)--;
            return 1;
        }
    return 0;
}
'''

SAMPLE_C_ARGS = r'''#include <stdio.h>
int main(int argc, char **argv)
{
    printf("ARGC %d\n", argc);
    for (int i = 1; i < argc; i++) printf("ARG %s\n", argv[i]);
    return 0;
}
'''
SAMPLE_C_NOARGS = 'int main(void) { return 0; }\n'
SAMPLE_CPP_ARGS = r'''#include <cstdio>
int main(int argc, char **argv)
{
    std::printf("ARGC %d\n", argc);
    for (int i = 1; i < argc; i++) std::printf("ARG %s\n", argv[i]);
    return 0;
}
'''
SAMPLE_CPP_NOARGS = 'int main() { return 0; }\n'
SAMPLE_CPP_CONST_ARGS = SAMPLE_CPP_ARGS.replace('char **argv', 'const char *argv[]')


def run(args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kw).stdout


def check(root, out):
    hook = out / 'provider'
    (hook / 'hook').mkdir(parents=True)
    (hook / 'common').mkdir()
    (hook / 'hook/ps3dk_run_hook.h').write_text(
        '#ifdef __cplusplus\nextern "C" {\n#endif\n'
        'int ps3dk_run_hook_start(int *, const char ***);\n'
        '#ifdef __cplusplus\n}\n#endif\n')
    (hook / 'hook/ps3dk_run_hook.c').write_text(HOOK_C)
    for name in ('net', 'sha256', 'hmac'):
        (hook / f'common/prn_{name}.c').write_text(f'int prn_{name}(int n) {{ return n + 1; }}\n')
    src = out / 'src'
    src.mkdir()
    samples = {'c_args': ('c_args.c', SAMPLE_C_ARGS), 'c_noargs': ('c_noargs.c', SAMPLE_C_NOARGS),
               'cpp_args': ('cpp_args.cpp', SAMPLE_CPP_ARGS), 'cpp_noargs': ('cpp_noargs.cpp', SAMPLE_CPP_NOARGS),
               'cpp_const_args': ('cpp_const_args.cpp', SAMPLE_CPP_CONST_ARGS)}
    for target, (name, text) in samples.items():
        (src / name).write_text(text)
    (src / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(hook_argv C CXX)\n'
        'add_library(net INTERFACE)\nadd_library(sysmodule_stub INTERFACE)\n'
        + ''.join(f'add_executable({t} {n})\n' for t, (n, _) in samples.items()))
    build = out / 'build'
    run(['cmake', '-S', str(src), '-B', str(build), '-G', 'Unix Makefiles',
         f'-DCMAKE_PROJECT_INCLUDE={root}/tests/hardware/psgl-sample-boot/run-hook-inject.cmake',
         f'-DPS3DK_RUN_HOOK_DIR={hook}'])
    run(['cmake', '--build', str(build), '--parallel', '2'])
    fails = 0
    for target in samples:
        r = subprocess.run([str(build / target), 'one', 'two words', MARKER], capture_output=True, text=True)
        lines = r.stdout.splitlines()
        if target.endswith('_args'):
            want = ['PSGL_SAMPLE_BOOT hook=1', 'ARGC 3', 'ARG one', 'ARG two words']
            ok = r.returncode == 0 and lines == want
        else:
            want = ['PSGL_SAMPLE_BOOT hook=1']
            ok = r.returncode == 0 and lines == want
        print(f'{"PASS" if ok else "FAIL"} {target}: rc {r.returncode}, saw {lines}' + ('' if ok else f', want {want}'))
        fails += not ok
    # tools/ps3dk-run/hook-check.py on these builds: a hooked target passes; naming a hooked target as the
    # module (which must have no hook) fails
    hc = root / 'tools' / 'ps3dk-run' / 'hook-check.py'
    for args, want_rc, what in (([str(build), 'c_args', 'c_args.c.o'], 0, 'hooked target passes'),
                                ([str(build), 'cpp_args', 'cpp_args.cpp.o'], 0, 'hooked C++ target passes'),
                                ([str(build), 'c_args', 'c_args.c.o', '--module', 'c_noargs'], 1,
                                 'a hooked "module" is refused')):
        r = subprocess.run([sys.executable, str(hc), *args, '--nm', 'nm'], capture_output=True, text=True)
        ok = r.returncode == want_rc
        print(f'{"PASS" if ok else "FAIL"} hook-check ({what}): rc {r.returncode}, want {want_rc}')
        if not ok:
            print(r.stdout + r.stderr)
        fails += not ok
    print('run-hook-inject-argv: ' + ('FAIL' if fails else 'PASS'))
    return fails


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory() as tmp:
        sys.exit(1 if check(root, Path(tmp)) else 0)
