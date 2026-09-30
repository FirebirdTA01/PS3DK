#!/usr/bin/env python3
"""Regenerate the public-name section of sdk/include/simdmath.h.

Each public simdmath name becomes a static inline function that calls the
per-arch inline implementation (_name), so calls inline, the address of a
function is available without -lsimdmath, and C++ code can name them as
std::name.  Signatures come from the prototypes in common/simdmath.h; the
set of names per arch comes from the #include lists already in the umbrella
(shared block, then the __SPU__ block).

usage: gen_inline_names.py            (rewrites ../include/simdmath.h)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTOS = os.path.join(HERE, 'common', 'simdmath.h')
UMBRELLA = os.path.join(HERE, '..', 'include', 'simdmath.h')
BEGIN = '/* ---- BEGIN generated public names (sdk/libsimdmath/gen_inline_names.py) ---- */'
END = '/* ---- END generated public names ---- */'
DBEGIN = '/* ---- BEGIN generated declarations (sdk/libsimdmath/gen_inline_names.py) ---- */'
DEND = '/* ---- END generated declarations ---- */'


def prototypes():
    text = open(PROTOS).read()
    out = {}
    for m in re.finditer(r'^([A-Za-z_][\w \*]*?)\b(\w+)\s*\(([^()]*)\)\s*;', text, re.M):
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3).strip()
        if name.startswith('_'):
            continue
        plist = [] if params in ('', 'void') else [p.strip() for p in params.split(',')]
        out[name] = (ret, plist)
    return out


def arch_names(umbrella):
    shared, spu = [], []
    in_spu = False
    for line in umbrella.splitlines():
        if line.startswith('/* ---- SPU-only inline implementations'):
            in_spu = True
        m = re.match(r'#include <simdmath/_(\w+)\.h>', line)
        if m and m.group(1) not in ('vec_utils', 'sincos'):
            (spu if in_spu else shared).append(m.group(1))
        if in_spu and line.startswith('#endif /* __SPU__ */'):
            break
    return shared, spu


TYPE_WORDS = {'vector', 'float', 'double', 'int', 'long', 'short', 'char', 'signed',
              'unsigned', 'void', 'const', 'volatile'}


def param_decl(ptype, i):
    # 'vector float *' -> 'vector float * a0'; a name the prototype gives
    # ('vector float x', 'vector signed int *quo') is replaced
    m = re.match(r'^(.*?[\s\*])([A-Za-z_]\w*)$', ptype)
    if m and m.group(2) not in TYPE_WORDS and not m.group(2).endswith('_t'):
        ptype = m.group(1).rstrip()
    return '%s a%d' % (ptype, i)


def declaration(name, protos):
    ret, plist = protos[name]
    decls = ', '.join(param_decl(p, i) for i, p in enumerate(plist)) or 'void'
    return 'static inline %s %s(%s);' % (ret, name, decls)


def wrapper(name, protos):
    if name not in protos:
        sys.exit('no prototype for %s in common/simdmath.h' % name)
    ret, plist = protos[name]
    decls = ', '.join(param_decl(p, i) for i, p in enumerate(plist)) or 'void'
    args = ', '.join('a%d' % i for i in range(len(plist)))
    body = ('_%s(%s);' % (name, args)) if ret == 'void' else ('return _%s(%s);' % (name, args))
    return 'static inline %s %s(%s) { %s }' % (ret, name, decls, body)


def main():
    umbrella = open(UMBRELLA).read()
    protos = prototypes()
    shared, spu = arch_names(umbrella)
    lines = [BEGIN,
             '/* Public names: static inline functions over the per-arch inlines.  A call',
             ' * inlines; taking the address needs no -lsimdmath; C++ also finds them as',
             ' * std::name. */']
    lines += [wrapper(n, protos) for n in shared]
    lines += ['', '#ifdef __SPU__']
    lines += [wrapper(n, protos) for n in spu]
    lines += ['#endif /* __SPU__ */', '',
              '#ifdef __cplusplus', 'namespace std {']
    lines += ['using ::%s;' % n for n in shared]
    lines += ['#ifdef __SPU__']
    lines += ['using ::%s;' % n for n in spu]
    lines += ['#endif', '}', '#endif', END]
    block = '\n'.join(lines)
    dlines = [DBEGIN,
              '/* Declared before the inline implementations: some of them call other',
              ' * public names (_atan2f4 calls divf4), which must resolve to these. */']
    dlines += [declaration(n, protos) for n in shared]
    dlines += ['#ifdef __SPU__'] + [declaration(n, protos) for n in spu] + ['#endif /* __SPU__ */', DEND]
    if DBEGIN not in umbrella:
        sys.exit('declaration markers missing in ' + UMBRELLA)
    pre, rest = umbrella.split(DBEGIN, 1)
    umbrella = pre + chr(10).join(dlines) + rest.split(DEND, 1)[1]
    if BEGIN in umbrella:
        pre, rest = umbrella.split(BEGIN, 1)
        post = rest.split(END, 1)[1]
        umbrella = pre + block + post
    else:
        sys.exit('markers missing in ' + UMBRELLA)
    open(UMBRELLA, 'w', newline='\n').write(umbrella)
    print('%d shared + %d SPU-only names' % (len(shared), len(spu)))


if __name__ == '__main__':
    main()
