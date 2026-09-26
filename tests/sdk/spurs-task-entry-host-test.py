#!/usr/bin/env python3
"""Execute the actual task-entry bridge on the host.

Only SPU intrinsics, exit and the shared SPURS headers task.h includes are
mocked.  The real shared headers size their records for the 32-bit SPU, so
a 64-bit host cannot compile them; the mocks give task.h just the names it
uses, and the bridge and task.h themselves are the real sources.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='spurs-task-entry-') as temp:
    work = Path(temp)
    (work/'spu_intrinsics.h').write_text(
        'typedef unsigned char qword __attribute__((vector_size(16)));\n'
        'typedef unsigned int vec_uint4 __attribute__((vector_size(16)));\n')
    spurs = work/'cell'/'spurs'
    spurs.mkdir(parents=True)
    for name in ('types.h', 'error.h', 'version.h', 'common.h'):
        (spurs/name).write_text('')
    (spurs/'task_types.h').write_text(
        '#include <stdint.h>\n'
        'typedef uint32_t CellSpursTaskId;\n'
        'typedef struct { uint32_t u32[4]; } CellSpursTaskLsPattern;\n'
        'typedef struct { unsigned char opaque[256]; } CellSpursTaskAttribute;\n'
        'typedef struct { unsigned char opaque[256]; } CellSpursTaskAttribute2;\n'
        '#define CELL_SPURS_TASK_TOP_MASK 0x0000ffffu\n'
        '#define CELL_SPURS_CONTEXT_SIZE2BITS(size) ((size) >> 11)\n'
        '#define CELL_SPURS_TASK2_REVISION 0\n'
        '#define _CELL_SPURS_INTERNAL_VERSION 0\n')
    (work/'probe.c').write_text(r'''
#include <cell/spurs/task.h>
#include <setjmp.h>
#include <string.h>
static jmp_buf done;
static qword expected;
static uint64_t expected_set;
static int calls, exits, code, wanted;
int cellSpursTaskMain(qword task, uint64_t set) {
    ++calls;
    if (memcmp(&task, &expected, sizeof(task)) || set != expected_set) return 999;
    return wanted;
}
void cellSpursTaskExit(int value) { ++exits; code = value; longjmp(done, 1); }
void cellSpursExit(void) { code = 998; longjmp(done, 1); }
int main(void) {
    const int values[] = {0, 37, -123};
    expected = (qword){0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,255};
    expected_set = UINT64_C(0x12345678abcdef01);
    for (int i=0; i<3; ++i) {
        wanted=values[i]; calls=exits=0;
        if (!setjmp(done)) { cellSpursMain(expected, expected_set); return 1; }
        if (calls != 1 || exits != 1 || code != wanted) return 2;
    }
    return 0;
}
''')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-D__SPU__', '-I'+str(work), '-I'+str(ROOT/'sdk/libspurs_task/include'),
                    os.environ.get('SPURS_TASK_BRIDGE_SOURCE', str(ROOT/'sdk/libspurs_task/src/spurs_task_main.c')), str(work/'probe.c'),
                    '-o', str(work/'probe')], check=True)
    subprocess.run([str(work/'probe')], check=True)
print('SPURS task entry arguments and signed exit code: PASS')
