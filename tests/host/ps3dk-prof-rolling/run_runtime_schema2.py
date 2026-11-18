#!/usr/bin/env python3
"""Build the genuine native-provider/schema2 runtime fixture (parent builds only).

No selector stub is linked: the test reports a named weak-symbol absence until
the private product seam exists. This command only builds; the explicit ignored
Rust target runs ordinary append truth FIRST, then rolling acceptance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
from run_runtime_spu import run, sha

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--cc', default='cc')
    p.add_argument('--sanitize', choices=('address', 'undefined'))
    p.add_argument('--public-options', action='store_true', help='exercise the public preset/setter/result')
    p.add_argument('--automatic-startup', action='store_true', help='link the real AUTO constructor with the public preset')
    p.add_argument('--omit-automatic-startup', action='store_true', help='sensitivity control: omit only that constructor')
    p.add_argument('--delayed-fifo', action='store_true', help='late FIFO accounting rows against retired windows')
    a = p.parse_args()
    if a.automatic_startup and not a.public_options or a.omit_automatic_startup and not a.automatic_startup:
        p.error('automatic startup requires public options; omission requires automatic startup')
    if a.delayed_fifo and (a.public_options or a.automatic_startup):
        p.error('delayed FIFO cases use the private schema2 selector only')
    if os.name != 'posix':
        raise RuntimeError('Linux/WSL host fixture only')
    out = a.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cc = shlex.split(a.cc)
    if not cc or shutil.which(cc[0]) is None:
        raise RuntimeError('compiler unavailable')
    lib = ROOT / 'sdk/libps3dkprof/src'
    gcm = ROOT / 'sdk/libgcm_cmd'
    public = out / 'include/ps3dk'
    public.mkdir(parents=True)
    public_inputs = [ROOT / 'sdk/include/ps3dk' / name for name in ('prof.h', 'prof_fifo.h', 'prof_auto.h')]
    # FIFO and AUTO include <ps3dk/prof_gpu.h>. Its canonical source is
    # installed from src/rsx, and it includes only prof.h plus system stdint.h.
    public_inputs += [lib / 'rsx/prof_gpu.h']
    if a.public_options:
        public_inputs += [ROOT/'sdk/include/ps3dk/prof_rolling.h']
    for src in public_inputs:
        shutil.copyfile(src, public / src.name)
    stub = out / 'native-port'
    for sub in ('rsx', 'sys'):
        (stub / sub).mkdir(parents=True, exist_ok=True)
    stubs = {
        'rsx/gcm_sys.h': '''#pragma once
#include <stdint.h>
typedef uint32_t u32;
struct _gcmCtxData;
typedef int32_t (*gcmContextCallback)(struct _gcmCtxData *,uint32_t);
typedef struct _gcmCtxData {u32 *begin,*end,*current;gcmContextCallback callback;} gcmContextData;
typedef struct {volatile u32 put,get,ref;} gcmControlRegister;
void *gcmGetControlRegister(void);
int32_t gcmAddressToOffset(void *,u32 *);
''',
        'sys/timer.h': '#pragma once\nint sys_timer_usleep(unsigned long);\n',
        'sys/systime.h': '#pragma once\n#include <stdint.h>\nuint64_t sysGetTimebaseFrequency(void);\n',
        'sys/lv2_types.h': '#pragma once\n',
        'ppu-asm.h': '#pragma once\n#define lv2_fn_to_callback_ea(f) ((uintptr_t)(f))\n',
        'ppu_host.h': '#include <stdint.h>\nuint64_t auto_test_timebase(void);\n#define __builtin_ppc_get_timebase() auto_test_timebase()\n',
    }
    for name, text in stubs.items():
        (stub / name).write_text(text)
    original = gcm / 'src/ps3tc_fifo_wrap.c'
    original_digest = sha(original)
    provider = out / 'ps3tc_fifo_wrap.host.c'
    text = original.read_text()
    # Only architecture instructions change. Observer registration, accounting,
    # exact PUT/current offsets, native wrap protocol and AUTO seams stay real.
    replacements = [
        ('__asm__ __volatile__ ("sync" ::: "memory");', '__atomic_thread_fence(__ATOMIC_SEQ_CST);'),
        ('__asm__ __volatile__("sync\\n\\tmftb %0\\n\\tisync"\n'
         '                         : "=r"(tb) : : "memory");',
         '__atomic_thread_fence(__ATOMIC_SEQ_CST);\n'
         '    tb=auto_test_timebase();\n    __atomic_thread_fence(__ATOMIC_SEQ_CST);'),
    ]
    for old, new in replacements:
        if text.count(old) != 1:
            raise RuntimeError('native architecture substitution drift')
        text = text.replace(old, new)
    provider.write_text(text)
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-prio-ctor-dtor', '-pthread',
             '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS',
             '-DPS3DK_PROF_WITH_SPU', '-DPS3DK_PROF_WITH_PROFILE', '-DPS3DK_PROF_WITH_FIFO',
             '-DPS3DK_PROF_WITH_AUTO', '-DPS3DK_PROF_AUTO_RING=1', '-DGPP_HOST_DEVMEM',
             '-DPS3DK_PROF_ROLLING', '-I', str(public.parent)]
    for name in ('core', 'rolling', 'pal', 'spu', 'rsx', 'fifo', 'auto_rsx'):
        flags += ['-I', str(lib / name)]
    flags += ['-I', str(gcm / 'src')]
    provider_flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                      '-DPS3TC_PROF_AUTO=1', '-DPS3TC_FIFO_OBSERVE=1', '-include', str(stub / 'ppu_host.h'),
                      '-I', str(stub), '-I', str(gcm / 'include'), '-I', str(gcm / 'src')]
    if a.sanitize:
        sanitizer = ['-fsanitize=' + ('address,undefined' if a.sanitize == 'address' else 'undefined'),
                     '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-ftls-model=local-exec']
        flags += sanitizer
        provider_flags += sanitizer
    sources = [lib / name for name in (
        'core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c',
        'rolling/roll_runtime.c', 'rolling/roll_store.c', 'rolling/roll_file.c', 'rolling/roll_output.c',
        'rolling/roll_ppu.c', 'rolling/roll_combined.c', 'rolling/roll_combined2.c', 'rolling/roll_retention_inspect.c',
        'spu/psp_ppu.c', 'spu/psp_spu.c', 'spu/ext_runtime.c', 'spu/psp_elf.c', 'spu/psp_host_emul.c',
        'rsx/gpu_runtime.c', 'rsx/prg.c', 'rsx/gpu_port_gcm.c', 'rsx/rsxcap.c',
        'fifo/fifo_runtime.c', 'fifo/fifo_writer.c',
        'auto_rsx/auto_runtime.c', 'auto_rsx/auto_config.c', 'auto_rsx/auto_wire.c', 'auto_rsx/auto_writer.c',
        'auto_rsx/auto_queue.c', 'auto_rsx/auto_commands.c', 'auto_rsx/auto_completion.c', 'auto_rsx/auto_frames.c',
        'auto_rsx/auto_clock.c', 'auto_rsx/auto_ring.c', 'auto_rsx/auto_ring_commands.c', 'auto_rsx/auto_ring_layout.c')]
    sources += [HERE / 'runtime_schema2_test.c']
    if a.public_options:
        flags += ['-DTEST_PUBLIC_OPTIONS','-DPS3DK_PROF_ROLLING_PUBLIC']
        sources += [lib/'rolling/roll_public.c']
    if a.automatic_startup:
        flags += ['-DTEST_AUTOMATIC_STARTUP']
        if not a.omit_automatic_startup:
            sources += [lib/'auto_rsx/auto_startup.c']
    if a.delayed_fifo:
        flags += ['-DTEST_DELAYED_FIFO']
    # No fake observer bridge. The automatic arm lets product constructor100
    # install options and initialize; the other arms explicitly call real init.
    wraps = ['ps3dk_prof__pal_alloc', 'ps3dk_prof__pal_open_excl', 'ps3dk_prof__pal_open_rw_excl',
             'ps3dk_prof__roll_combined2_init', 'ps3dk_prof__roll_output_accept',
             'ps3dk_prof__fifo_emit_chunk', 'ps3dk_prof__auto_queue_ack', 'ps3dk_prof__auto_ring_ack']
    if a.delayed_fifo:
        wraps += ['ps3dk_prof__roll_combined2_prepare']  # retired-frontier oracle (test link wrap)
    obj = out / 'native-provider.o'
    exe = out / 'runtime-schema2'
    commands = [cc + provider_flags + ['-c', str(provider), '-o', str(obj)],
                cc + flags + list(map(str, sources)) + [str(obj)] +
                ['-Wl,--wrap=' + x for x in wraps] + ['-o', str(exe)]]
    seeds = set(sources + public_inputs + [original, provider, Path(__file__).resolve(),
                HERE / 'run_runtime_spu.py', HERE.parent / 'ps3dk-prof-runtime/auto-ring-runtime-test.c'])
    seeds.update(p for p in stub.rglob('*') if p.is_file())
    pins = {str(p.resolve()): sha(p) for p in seeds}
    deps = set()
    for i, (source, own_flags) in enumerate([(provider, provider_flags)] + [(s, flags) for s in sources]):
        label = f'deps-{i:02}'
        if run(cc + own_flags + ['-M', '-MT', 'input', str(source)], out, label, 60):
            raise RuntimeError(f'{label}: dependency discovery failed; not behavioral RED')
        data = (out / (label + '.stdout')).read_text().replace('\\\n', ' ')
        if not data.startswith('input:'):
            raise RuntimeError('dependency inventory format')
        deps.update(Path(s).resolve() for s in shlex.split(data.split(':', 1)[1]))
    if any(sha(Path(p)) != digest for p, digest in pins.items()):
        raise RuntimeError('source changed during dependency discovery')
    deps.add(Path(shutil.which(cc[0])).resolve())
    pins.update({str(p): sha(p) for p in deps})
    receipt = dict(inputs=pins, commands=commands,
                   command_sha256=hashlib.sha256(json.dumps(commands).encode()).hexdigest(),
                   native_original_sha256=original_digest, native_host_sha256=sha(provider),
                   architecture_substitutions=replacements, stages=[])
    (out / 'inputs.json').write_text(json.dumps(receipt, indent=2) + '\n')
    for i, command in enumerate(commands):
        if any(sha(Path(p)) != digest for p, digest in pins.items()):
            raise RuntimeError('input changed before compiler stage')
        rc = run(command, out, f'compile-{i}', 180)
        stable = all(sha(Path(p)) == digest for p, digest in pins.items())
        receipt['stages'].append(dict(rc=rc, inputs_unchanged=stable))
        if not rc:
            receipt['stages'][-1]['output_sha256'] = sha(obj if i == 0 else exe)
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if rc or not stable:
            raise RuntimeError('compile/input gate failed; not behavioral RED')
    print(f'PS3DK_PROF_RUNTIME_SCHEMA2={exe}', flush=True)


if __name__ == '__main__':
    main()
