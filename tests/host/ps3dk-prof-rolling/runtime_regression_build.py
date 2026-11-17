"""Shared bounded build recipe for new actual-runtime regression witnesses."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
from run_runtime_spu import run, sha
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]

def build(out, cc_text, sanitize, *, spu=False, live=False):
    if os.name!='posix': raise RuntimeError('Linux/WSL build only; caller owns slot')
    out=out.resolve();out.mkdir(parents=True,exist_ok=False)
    lib=ROOT/'sdk/libps3dkprof/src';inc=out/'include/ps3dk';inc.mkdir(parents=True)
    shutil.copyfile(ROOT/'sdk/include/ps3dk/prof.h',inc/'prof.h')
    cc=shlex.split(cc_text)
    if not cc or not shutil.which(cc[0]): raise RuntimeError('compiler unavailable')
    flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-DPS3DK_PROF=1',
           '-DPS3DK_PROF_PAL_HOST','-DPS3DK_PROF_TEST_HOOKS','-DPS3DK_PROF_ROLLING','-I',str(inc.parent)]
    for name in ('core','rolling','pal','spu','rsx','live'): flags+=['-I',str(lib/name)]
    if spu: flags+=['-DPS3DK_PROF_WITH_SPU','-DPS3DK_PROF_WITH_PROFILE']
    if sanitize: flags+=['-fsanitize='+('address,undefined' if sanitize=='address' else 'undefined'),
                        '-fno-omit-frame-pointer','-fno-sanitize-recover=all','-ftls-model=local-exec']
    names=['core/prof_core.c','core/prof_drain.c','core/prof_version.c','pal/pal_host.c',
           'rolling/roll_runtime.c','rolling/roll_store.c','rolling/roll_file.c','rolling/roll_output.c',
           'rolling/roll_ppu.c','rolling/roll_retention_inspect.c']
    wraps=['ps3dk_prof__pal_open_excl','ps3dk_prof__pal_open_rw_excl','ps3dk_prof__roll_output_accept']
    if spu:
        names+=['rolling/roll_combined.c','spu/psp_ppu.c','spu/psp_spu.c','spu/ext_runtime.c',
                'spu/psp_elf.c','spu/psp_host_emul.c','rsx/gpu_runtime.c','rsx/prg.c','rsx/gpu_port_gcm.c']
        wraps+=['malloc','calloc','realloc','free','posix_memalign','aligned_alloc',
                'ps3dk_prof__roll_combined_init','ps3dk_prof_test_rolling_combined_set']
    else:
        wraps+=['ps3dk_prof__pal_close','ps3dk_prof__pal_alloc','ps3dk_prof__pal_thread_create',
                'ps3dk_prof__roll_ppu_init']
    if live:
        names+=['live/live_api.c','live/live_agent.c','live/live_os_host.c']
        wraps+=['malloc','calloc','realloc','ps3dk_prof__live_agent_start','ps3dk_prof__live_os_listen']
        fixture=HERE/'runtime_live_rolling_test.c';name='runtime-live-rolling'
    else:
        wraps+=['ps3dk_prof__pal_retain_complete_kind','ps3dk_prof__roll_retention_inspect',
                'ps3dk_prof_shutdown',
                'ps3dk_prof__roll_combined_prepare' if spu else 'ps3dk_prof__roll_ppu_prepare']
        fixture=HERE/'runtime_retention_budget_test.c';name='runtime-retention-'+('spu' if spu else 'ppu')
    files=[lib/n for n in names]+[fixture];exe=out/name
    command=cc+flags+list(map(str,files))+['-Wl,--wrap='+n for n in wraps]+['-o',str(exe)]
    # Snapshot this build's inputs only. Unrelated encoder sources may be under
    # development concurrently; transitive headers are added from real -M
    # dependencies below, checked again before and after the actual compile.
    paths={p.resolve() for p in files}
    paths.update((HERE/'run_runtime_spu.py',HERE/'runtime_regression_build.py',
                  HERE/('run_runtime_live_rolling.py' if live else 'run_runtime_retention_budget.py'),
                  HERE/('runtime_spu_test.c' if spu else 'runtime_ppu_test.c'),
                  ROOT/'sdk/include/ps3dk/prof.h',inc/'prof.h',
                  Path(shutil.which(cc[0])).resolve()))
    if spu:
        paths.update((HERE/'runtime_spu_test_api.h',HERE.parent/'ps3dk-prof-spu-int/i2_test.c'))
    pins={str(p):sha(p) for p in paths}
    for i,source in enumerate(files):
        label=f'deps-{i:02}'
        if run(cc+flags+['-M','-MT','input',str(source)],out,label,60): raise RuntimeError('dependency discovery failed, not RED')
        text=(out/(label+'.stdout')).read_text().replace('\\\n',' ')
        if not text.startswith('input:'): raise RuntimeError('dependency format')
        for p in shlex.split(text.split(':',1)[1]):
            path=Path(p).resolve();key=str(path)
            if key in pins and pins[key]!=sha(path): raise RuntimeError('source raced during dependency discovery')
            pins[key]=sha(path)
    if any(sha(Path(p))!=h for p,h in pins.items()): raise RuntimeError('source changed before compile')
    receipt=dict(command=command,command_sha256=hashlib.sha256(json.dumps(command).encode()).hexdigest(),inputs=pins)
    (out/'inputs.json').write_text(json.dumps(receipt,indent=2)+'\n')
    rc=run(command,out,'compile',180)
    stable=all(sha(Path(p))==h for p,h in pins.items());receipt.update(compile_rc=rc,inputs_unchanged=stable)
    if not rc: receipt['executable_sha256']=sha(exe)
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    if rc or not stable: raise RuntimeError('compile/input gate failed; not behavioral RED')
    return exe
