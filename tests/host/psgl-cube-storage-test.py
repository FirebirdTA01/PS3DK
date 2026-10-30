#!/usr/bin/env python3
"""Compile production cube storage helpers against allocation/completion faults."""
import os
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=ROOT / 'sdk/libPSGL/src/psgl_context.c')
source = parser.parse_args().source.read_text()


def block(text, start):
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    return text[start:end]


def function(name):
    match = re.search(r'(?m)^static [^\n]+\b' + name + r'\([^;]*?\)\n\{', source)
    if not match:
        raise SystemExit('missing production helper: ' + name)
    return block(source, match.start())


names = ['psgl_is_power_of_two', 'psgl_texture_remap_argb', 'psgl_texture_remap_dxt_rb',
         'psgl_texture_is_dxt', 'psgl_texture_is_depth', 'psgl_release_texture_storage',
         'psgl_texture_base_format', 'psgl_texel_size', 'psgl_unpack_row_stride',
         'psgl_read_u16_be', 'psgl_read_u32_be', 'psgl_pack_argb8', 'psgl_convert_texel_argb8',
         'psgl_fill_gcm_texture', 'psgl_cube_layout', 'psgl_cube_complete',
         'psgl_cube_set_face', 'psgl_cube_prepare', 'psgl_cube_release']
code = '\n\n'.join(map(function, names))
extensions = (ROOT / 'sdk/libPSGL/src/core_gl_ext.c').read_text()
code += '\n\n' + block(extensions, extensions.index('GLAPI void glGenerateMipmapOES('))
types = []
for path, name in [('sdk/include/cell/gcm.h', 'CellGcmTexture'),
                   ('sdk/libPSGL/src/psgl_context.c', 'PSGLcubeFace'),
                   ('sdk/libPSGL/src/psgl_context.c', 'PSGLtextureObject')]:
    text = (ROOT / path).read_text()
    types.append(block(text, text.index('typedef struct ' + name + ' {')) + ' ' + name + ';')
# Pull only the real GCM constants needed by these helpers, including macro
# dependencies. This avoids target-only syscall inline assembly on the host.
definitions = {}
for path in ['sdk/include/rsx/gcm_sys.h', 'sdk/include/cell/gcm/gcm_enum.h']:
    text = (ROOT / path).read_text().replace('\\\n', ' ')
    for line in text.splitlines():
        match = re.match(r'#define\s+((?:CELL_)?GCM_\w+)\b', line)
        if match: definitions[match[1]] = line
needed = set(re.findall(r'\b(?:CELL_)?GCM_\w+', code)) - {'CELL_GCM_CURRENT'}
while True:
    expanded = needed | set(re.findall(r'\b(?:CELL_)?GCM_\w+', '\n'.join(definitions[n] for n in needed)))
    if expanded == needed: break
    needed = expanded
types += [definitions[n] for n in sorted(needed)]
types.append('#define CELL_GCM_CURRENT NULL')
context_header = (ROOT / 'sdk/libPSGL/src/psgl_context.h').read_text()
types.append('enum {\n' + '\n'.join(line for line in context_header.splitlines() if re.match(r'\s+PSGL_DIRTY_\w+ =', line)) + '\n};')
types.append('#define PSGL_MAX_TEXTURE_UNITS 8')
types.extend(line for line in (ROOT / 'sdk/libPSGL/src/ffp_emit.h').read_text().splitlines()
             if re.match(r'#define PSGL_FFP_(LIGHTING_MASK|TEXUNIT_SHIFT|TEXUNITS_MASK)\b', line))
with tempfile.TemporaryDirectory(prefix='psgl-cube-') as directory:
    tmp = Path(directory)
    (tmp / 'cube-types.h').write_text('\n'.join(types))
    (tmp / 'cube-production.h').write_text(code)
    (tmp / 'cube-boundary.h').write_text(function('psgl_validate_sampler_units') + '\n' + function('psgl_validate_draw_state'))
    exe = tmp / 'cube'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-I' + str(ROOT / 'sdk/include'), '-I' + str(tmp),
                    str(ROOT / 'tests/host/psgl-cube-storage-test.c'), '-o', str(exe)], check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
