#!/usr/bin/env python3
"""Pre-link identities for PS3_PROF_AUTO executables, and their external manifest.

prepare() runs before the link. It finds every file the linker reads, by a trial link of the same command with a
placeholder identity object and -Wl,--verbose: positional objects, -l libraries, the driver's default startup
files and libraries, and linker scripts with the files they name. It hashes those, the response files and any
-specs files (the output excluded) into the build binding, hashes the installed provider manifest into the
mechanism digest, writes and compiles one identity object that defines ps3dk_prof__auto_mechanism,
ps3dk_prof__auto_build_binding, ps3dk_prof__auto_build_id and ps3dk_prof__auto_app, and returns the link command
with that object appended. Inputs the linker reads without tracing them (a response file passed through to the
linker) are refused. finish() runs after the link: it refuses if any input or the identity object changed, or if
the link map loads a file that is not in the binding, then records the linked ELF's sha256 next to the map.
bind_self() records the signed SELF made from that exact ELF. The capture carries the binding; the manifest ties
the binding to the ELF and SELF, so nothing hashes itself.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

SCHEMA = 1
# The command/report semantics the binding is bound to: the AUTO wire with segment and frame-mark records.
MECHANISM_TAG = 'ps3dk-prof AUTO RSX schema 1; CONFIG masks 31/11; SEGMENT and FRAME records; PRESENT rows'
ABIS = ('ilp32', 'lp64')


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def _expand(argv, seen):
    """argv with driver response files expanded; returns (arguments, response files). An @file that is the value
    of -Xlinker belongs to the linker, not the driver, and is left for _unresolved to refuse."""
    out, responses = [], []
    for k, arg in enumerate(argv):
        if arg.startswith('@') and not (k and argv[k - 1] == '-Xlinker'):
            path = Path(arg[1:]).resolve()
            if path in seen:
                raise ValueError('recursive response file '+str(path))
            if not path.is_file():
                raise ValueError('missing input (response file) '+str(path))
            inner, more = _expand(shlex.split(path.read_text()), seen | {path})
            responses += [path, *more]
            out += inner
        else:
            out.append(arg)
    return out, responses


# Driver options whose value is the next argument: that value is not a positional input file.
_SEPARATE = frozenset(('-L', '-l', '-T', '-Xlinker', '-u', '-e', '-B', '-x', '-specs', '--specs', '-z',
                       '--sysroot', '-isysroot', '-include', '-imacros', '-MF', '-MT', '-MQ'))


def _inputs(args):
    """(input files, output) of a compiler-driver link command (args without argv[0])."""
    inputs, output, i = [], None, 0
    while i < len(args):
        a = args[i]
        if a in _SEPARATE:
            i += 2
            continue
        if a == '-o':
            if i + 1 >= len(args):
                raise ValueError('one output required (-o without a path)')
            if output is not None:
                raise ValueError('one output required (two -o)')
            output = args[i + 1]
            i += 2
            continue
        if a.startswith('-o') and len(a) > 2:
            if output is not None:
                raise ValueError('one output required (two -o)')
            output = a[2:]
        elif not a.startswith('-'):
            p = Path(a)
            if not p.is_file():
                raise ValueError('missing input '+a)
            inputs.append(p.resolve())
        i += 1
    if output is None:
        raise ValueError('one output required (no -o)')
    return inputs, Path(output).resolve()


_OPENED = re.compile(r'^attempt to open (.+) succeeded$|^opened script file (.+)$')
_LOAD = re.compile(r'^LOAD (.+)$')


def _unresolved(args):
    """Refuse linker inputs that the trial link's trace cannot see."""
    for i, a in enumerate(args):
        if a.startswith('-Wl,') and any(w.startswith('@') for w in a[4:].split(',')):
            raise ValueError('unresolved link input (linker response file): '+a)
        if a == '-Xlinker' and i + 1 < len(args) and args[i + 1].startswith('@'):
            raise ValueError('unresolved link input (linker response file): -Xlinker '+args[i + 1])


def _specs(args):
    """Driver specs files named on the command line; each must exist."""
    out = []
    for i, a in enumerate(args):
        name = None
        if a.startswith(('-specs=', '--specs=')):
            name = a.split('=', 1)[1]
        elif a in ('-specs', '--specs') and i + 1 < len(args):
            name = args[i + 1]
        if name is not None:
            p = Path(name)
            if not p.is_file():
                raise ValueError('unresolved link input (specs file): '+name)
            out.append(p.resolve())
    return out


def _resolve(path, cwd):
    p = Path(path)
    return (p if p.is_absolute() else Path(cwd) / p).resolve()


def _compile_identity(driver, abi, source, obj, text):
    source.write_text('/* Generated by ps3-prof-auto-identity.py before the link; do not edit. */\n'+text)
    # The driver may be a C++ driver (g++): compile the identity source as C explicitly.
    compile_cmd = [driver, *(['-mlp64'] if abi == 'lp64' else []), '-x', 'c', '-c', str(source), '-o', str(obj)]
    r = subprocess.run(compile_cmd, capture_output=True, text=True, timeout=300)
    if r.returncode:
        raise ValueError('identity object did not compile: '+r.stderr)


def _traced_inputs(argv, placeholder, cwd):
    """Every file the linker opens for this command, from a trial link with the placeholder identity object."""
    r = subprocess.run([*argv, str(placeholder), '-Wl,--verbose'], capture_output=True, text=True, timeout=1800,
                       cwd=cwd)
    if r.returncode:
        raise ValueError('trial link failed, so its inputs cannot be resolved: '+(r.stderr or r.stdout)[-2000:])
    opened = set()
    for line in (r.stdout+'\n'+r.stderr).splitlines():
        m = _OPENED.match(line.strip())
        if m:
            opened.add(_resolve(m.group(1) or m.group(2), cwd))
    if not opened:
        raise ValueError('trial link printed no input trace (a linker without GNU ld --verbose output?)')
    return opened


def _manifest_path(mapfile):
    return Path(str(mapfile)+'.identity.json')


def _c_bytes(hexstr):
    return ','.join('0x'+hexstr[i:i+2] for i in range(0, len(hexstr), 2))


def prepare(abi, install_manifest, mapfile, argv):
    if abi not in ABIS:
        raise ValueError('ABI '+str(abi))
    argv = [str(a) for a in argv]
    if not argv:
        raise ValueError('empty link command')
    install = json.loads(Path(install_manifest).read_text())
    if install.get('schema') != 1 or install.get('abi') != abi or not isinstance(install.get('archives'), dict):
        raise ValueError('ABI or schema of the install manifest does not match '+abi)
    args, responses = _expand(argv[1:], frozenset())
    if ('-mlp64' in args) != (abi == 'lp64'):
        raise ValueError('ABI of the link command does not match '+abi)
    files, output = _inputs(args)
    _unresolved(args)
    specs = _specs(args)
    cwd = os.getcwd()
    mapfile = Path(mapfile).resolve()
    source = Path(str(mapfile)+'.identity.c')
    obj = Path(str(mapfile)+'.identity.o')
    trial_source = Path(str(mapfile)+'.identity-trial.c')
    trial_obj = Path(str(mapfile)+'.identity-trial.o')
    # The extern declarations keep the definitions external even if a driver compiles this file as C++, where a
    # namespace-scope const object otherwise has internal linkage.
    decls = ('extern const unsigned char ps3dk_prof__auto_mechanism[32];\n'
             'extern const unsigned char ps3dk_prof__auto_build_binding[32];\n'
             'extern const char ps3dk_prof__auto_build_id[65];\n'
             'extern const char ps3dk_prof__auto_app[];\n')
    zero = '0'*64
    _compile_identity(argv[0], abi, trial_source, trial_obj, decls+'\n'+
                      'const unsigned char ps3dk_prof__auto_mechanism[32]={'+_c_bytes(zero)+'};\n'
                      'const unsigned char ps3dk_prof__auto_build_binding[32]={'+_c_bytes(zero)+'};\n'
                      'const char ps3dk_prof__auto_build_id[65]="'+zero+'";\n'
                      'const char ps3dk_prof__auto_app[]="trial";\n')
    traced = _traced_inputs(argv, trial_obj, cwd)
    untraced = sorted(str(f) for f in files if f not in traced)
    if untraced:
        raise ValueError('trial link did not open named inputs: '+', '.join(untraced))
    inputs = sorted({*map(str, traced), *map(str, responses), *map(str, specs)} - {str(output), str(trial_obj.resolve())})
    digests = {p: sha(p) for p in inputs}
    mechanism = hashlib.sha256(json.dumps(dict(tag=MECHANISM_TAG, abi=abi, archives=install['archives']),
                                          sort_keys=True).encode()).hexdigest()
    binding = hashlib.sha256(json.dumps(dict(schema=SCHEMA, abi=abi, mechanism=mechanism, argv=argv,
                                             inputs=digests), sort_keys=True).encode()).hexdigest()
    app = output.name.split('.')[0][:32] or 'app'
    defs = ('const unsigned char ps3dk_prof__auto_mechanism[32]={'+_c_bytes(mechanism)+'};\n'
            'const unsigned char ps3dk_prof__auto_build_binding[32]={'+_c_bytes(binding)+'};\n'
            'const char ps3dk_prof__auto_build_id[65]="'+binding+'";\n'
            'const char ps3dk_prof__auto_app[]="'+''.join(c for c in app if c.isalnum() or c in '-_')+'";\n')
    _compile_identity(argv[0], abi, source, obj, decls+'\n'+defs)
    return dict(schema=SCHEMA, abi=abi, mechanism=mechanism, binding=binding, inputs=inputs, input_sha=digests,
                cwd=cwd, output=str(output), identity_object=str(obj), identity_sha=sha(obj),
                command=[*argv, str(obj)])


def finish(prepared, mapfile, require_map=False):
    for path, digest in prepared['input_sha'].items():
        if not Path(path).is_file() or sha(path) != digest:
            raise ValueError('input changed during the link: '+path)
    if sha(prepared['identity_object']) != prepared['identity_sha']:
        raise ValueError('identity object changed during the link')
    mapfile = Path(mapfile).resolve()
    if require_map:
        if not mapfile.is_file():
            raise ValueError('link map missing: '+str(mapfile))
        bound = set(prepared['input_sha']) | {str(Path(prepared['identity_object']).resolve())}
        loads = [_resolve(m.group(1).strip(), prepared['cwd']) for m in
                 map(_LOAD.match, mapfile.read_text(errors='replace').splitlines()) if m]
        if not loads:
            raise ValueError('link map lists no LOAD inputs: '+str(mapfile))
        extra = sorted({str(p) for p in loads} - bound)
        if extra:
            raise ValueError('link loaded inputs that are not in the binding: '+', '.join(extra))
    elf = Path(prepared['output'])
    result = dict(schema=SCHEMA, abi=prepared['abi'], binding=prepared['binding'], build_id=prepared['binding'],
                  mechanism=prepared['mechanism'], elf=dict(path=str(elf), sha256=sha(elf)), packaged=False,
                  inputs=prepared['input_sha'])
    _manifest_path(Path(mapfile).resolve()).write_text(json.dumps(result, indent=2)+'\n')
    return result


def bind_self(mapfile, self_path):
    path = _manifest_path(Path(mapfile).resolve())
    result = json.loads(path.read_text())
    if sha(result['elf']['path']) != result['elf']['sha256']:
        raise ValueError('ELF changed since the link; the SELF cannot be bound')
    result['self'] = dict(path=str(Path(self_path).resolve()), sha256=sha(self_path))
    result['packaged'] = True
    path.write_text(json.dumps(result, indent=2)+'\n')
    return result
