#!/usr/bin/env python3
"""Run production time/sched code on host with only Lv-2/newlib dependencies supplied."""
import os
from pathlib import Path
import subprocess
import tempfile
import resource
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='ps3dk-time-') as temp:
    work = Path(temp)
    def put(name, text):
        p = work / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    put('sys/systime.h', '#pragma once\n#include <stdint.h>\ntypedef uint64_t u64; typedef int64_t s64; typedef uint32_t u32; typedef int32_t s32;\ns64 sysGetSystemTime(void); s32 sysGetCurrentTime(u64*,u64*); s32 sysUsleep(u32);\n')
    put('ppu-types.h', '#include <sys/systime.h>\n')
    put('sys/reent.h', '#pragma once\nstruct _reent { int _errno; };\n')
    put('sys/lv2errno.h', '#include <sys/systime.h>\n#include <sys/reent.h>\ns32 lv2error(s32); s32 lv2errno(s32); s32 lv2errno_r(struct _reent*,s32);\n')
    # newlib attributes are public structs; host pthread_attr_t is opaque.
    # Cross checks independently compile against the actual newlib layout.
    put('pthread.h', '#pragma once\n#include <sched.h>\ntypedef struct { int is_initialized, schedpolicy, inheritsched; } ps3test_attr_t;\n#define pthread_attr_t ps3test_attr_t\n#define PTHREAD_INHERIT_SCHED 1\n#define PTHREAD_EXPLICIT_SCHED 2\nint pthread_attr_setschedpolicy(pthread_attr_t*,int);\nint pthread_attr_setinheritsched(pthread_attr_t*,int);\n')
    put('time.h', '#pragma once\n#undef clock_gettime\n#undef nanosleep\n#include_next <time.h>\n#define clock_gettime ps3test_clock_gettime\n#define nanosleep ps3test_nanosleep\nint clock_gettime(clockid_t,struct timespec*);\nint nanosleep(const struct timespec*,struct timespec*);\n')
    sources = ['tests/sdk/posix-time-host-test.c', 'runtime/lv2/librt/posix_time.c',
               'runtime/lv2/librt/pthread_sched.c', 'runtime/lv2/librt/lv2errno.c']
    cmd = [os.environ.get('CC','cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
           '-Dclock_gettime=ps3test_clock_gettime', '-Dnanosleep=ps3test_nanosleep',
           '-Dpthread_attr_setschedpolicy=ps3test_setschedpolicy',
           '-Dpthread_attr_setinheritsched=ps3test_setinheritsched',
           '-Wall', '-Wextra', '-Werror', '-I'+str(work),
           *[os.environ.get('POSIX_TIME_SOURCE', str(ROOT/s)) if s.endswith('/posix_time.c') else str(ROOT/s) for s in sources], '-o', str(work/'test')]
    subprocess.run(cmd, check=True)
    subprocess.run([str(work/'test')], check=True)
