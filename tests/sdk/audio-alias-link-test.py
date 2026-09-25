#!/usr/bin/env python3
"""Both-ABI audio import ownership controls; never runs target code.

With --nidgen, builds only the audio archive into --output. Otherwise checks
the installed SDK (useful as a released-archive RED control).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--ps3dev', type=Path, default=os.environ.get('PS3DEV'))
p.add_argument('--nidgen', type=Path)
p.add_argument('--output', type=Path, default=ROOT/'build/audio-alias-test')
args = p.parse_args()
if not args.ps3dev:
    print('audio alias link: SKIP (PS3DEV not set)')
    raise SystemExit(0)
dev = args.ps3dev.resolve()
sdk = dev/'ps3dk'
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PS3DEV=str(dev), PS3DK=str(sdk), PSL1GHT=str(sdk))
cc = dev/'ppu/bin/powerpc64-ps3-elf-gcc'


def elf_symbols(path):
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x02\x02', 'expected ELF64 big-endian'
    offset = struct.unpack_from('>Q', data, 40)[0]
    width, count = struct.unpack_from('>HH', data, 58)
    sections = [struct.unpack_from('>IIQQQQIIQQ', data, offset+i*width) for i in range(count)]
    shstrings = sections[struct.unpack_from('>H', data, 62)[0]]
    shnames = data[shstrings[4]:shstrings[4]+shstrings[5]]
    section_names = [shnames[s[0]:shnames.index(0, s[0])].decode() for s in sections]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4]+strings[5]]
        for pos in range(section[4], section[4]+section[5], section[9]):
            name, info, other, index, value, size = struct.unpack_from('>IBBHQQ', data, pos)
            if not name or not index:
                continue
            label = names[name:names.index(0, name)].decode()
            symbols[label] = (info >> 4, index, value, size)

    def descriptor(name):
        binding, index, value, size = symbols[name]
        assert index < len(sections) and section_names[index] == '.opd', name
        section = sections[index]
        pos = section[4] + value - section[3]
        # GCC records function code size on an application's OPD symbol;
        # nidgen records descriptor size. Both use the compact 8-byte OPD.
        assert section[4] <= pos <= section[4]+section[5]-8, name
        return struct.unpack_from('>II', data, pos)

    def code_section(address):
        return next((section_names[i] for i, s in enumerate(sections)
                     if s[2] & 4 and s[3] <= address < s[3]+s[5]), None)
    return symbols, descriptor, code_section


source = out/'owner.c'
source.write_text('''extern int cellAudioInit(void), cellAudioQuit(void);
extern int audioInit(void), audioQuit(void);
#ifdef APP_INIT
int audioInit(void) { return 31; }
#endif
#ifdef APP_QUIT
int audioQuit(void) { return 37; }
#endif
#ifdef DUP_CANONICAL
int cellAudioInit(void) { return 41; }
#endif
int main(void) {
    int r = 0;
#ifdef CANONICAL
    r += cellAudioInit() + cellAudioQuit();
#endif
#ifdef LEGACY
    r += audioInit() + audioQuit();
#endif
    return r;
}
''')
rows = []
for abi, flags in [('ilp32', []), ('lp64', ['-mlp64'])]:
    if args.nidgen:
        archive_dir = out/abi
        subprocess.run([str(args.nidgen.resolve()), 'archive', '--input',
                        str(ROOT/'tools/nidgen/nids/extracted/libaudio_stub.yaml'),
                        '--toolchain-bin', str(dev/'ppu/bin'), '--abi', abi,
                        '--out-dir', str(archive_dir)], check=True)
    else:
        archive_dir = sdk/'ppu/lib'/('lp64' if abi == 'lp64' else '')
    archive = archive_dir/'libaudio_stub.a'
    for case, defines in [
        ('canonical', ['CANONICAL']), ('legacy', ['LEGACY']),
        ('app-init', ['APP_INIT', 'CANONICAL', 'LEGACY']),
        ('app-quit', ['APP_QUIT', 'CANONICAL', 'LEGACY']),
        ('app-both', ['APP_INIT', 'APP_QUIT', 'CANONICAL', 'LEGACY']),
        ('duplicate-app-init', ['APP_INIT', 'CANONICAL', 'LEGACY']),
        ('duplicate-app-quit', ['APP_QUIT', 'CANONICAL', 'LEGACY']),
        ('duplicate-canonical', ['DUP_CANONICAL', 'CANONICAL'])]:
        stem = out/f'{abi}-{case}'
        app_obj = stem.with_suffix('.o')
        compile_cmd = [str(cc), *flags, '-O0', '-mcpu=cell', str(source),
                       *['-D'+x for x in defines], '-c', '-o', str(app_obj)]
        subprocess.run(compile_cmd, env=env, check=True)
        objects = [str(app_obj)]
        collision = 'cellAudioInit' if case == 'duplicate-canonical' else None
        if case.startswith('duplicate-app-'):
            collision = 'audioInit' if case.endswith('init') else 'audioQuit'
            duplicate = stem.with_suffix('.duplicate.c')
            duplicate.write_text(f'int {collision}(void) {{ return 43; }}\n')
            duplicate_obj = stem.with_suffix('.duplicate.o')
            subprocess.run([str(cc), *flags, '-mcpu=cell', '-c', str(duplicate),
                            '-o', str(duplicate_obj)], env=env, check=True)
            objects.append(str(duplicate_obj))
        cmd = [str(cc), *flags, *objects, str(archive),
               '-Wl,-Map,'+str(stem.with_suffix('.map'))+',--cref',
               '-o', str(stem.with_suffix('.elf'))]
        result = subprocess.run(cmd, env=env, text=True, capture_output=True)
        stem.with_suffix('.log').write_text(result.stdout+result.stderr)
        row = dict(abi=abi, case=case, compile_command=compile_cmd, command=cmd, rc=result.returncode,
                   archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        errors = []
        if collision:
            if not result.returncode or 'multiple definition' not in result.stderr or collision not in result.stderr:
                errors.append(collision+' collision was not rejected')
        elif result.returncode:
            errors.append('link failed')
        else:
            try:
                symbols, descriptor, code_section = elf_symbols(stem.with_suffix('.elf'))
                crossref = stem.with_suffix('.map').read_text().split('Cross Reference Table', 1)[1].splitlines()
                def owner(symbol):
                    return next((line.split(None, 1)[1].strip() for line in crossref
                                 if line.startswith(symbol+' ')), 'MISSING')
                row['owners'] = {s: owner(s) for s in ['audioInit', 'audioQuit', 'cellAudioInit', 'cellAudioQuit']}
                for suffix, define in [('Init', 'APP_INIT'), ('Quit', 'APP_QUIT')]:
                    canonical, legacy, trampoline = 'cellAudio'+suffix, 'audio'+suffix, '__cellAudio'+suffix
                    assert symbols[canonical][0] == 1, 'canonical must remain global'
                    assert owner(canonical) == str(archive)+'(audio.o)', 'canonical not archive-owned'
                    assert descriptor(canonical)[0] == symbols[trampoline][2], 'canonical import redirected'
                    assert code_section(descriptor(canonical)[0]) == '.sceStub.text', 'canonical not import code'
                    if define in defines:
                        assert symbols[legacy][0] == 1, 'app definition did not win'
                        assert owner(legacy) == str(app_obj), 'alias not app-owned'
                        assert code_section(descriptor(legacy)[0]) == '.text', 'app entry not in app text'
                        assert descriptor(legacy)[0] != descriptor(canonical)[0], 'app redirected to import'
                    else:
                        assert symbols[legacy][0] == 2, 'legacy fallback must be weak'
                        assert owner(legacy) == str(archive)+'(audio.o)', 'fallback not archive-owned'
                        assert descriptor(legacy) == descriptor(canonical), 'legacy fallback differs from import'
                row['elf_sha256'] = hashlib.sha256(stem.with_suffix('.elf').read_bytes()).hexdigest()
            except (AssertionError, KeyError, IndexError) as error:
                errors.append(str(error))
        row['errors'] = errors
        rows.append(row)
        print(abi, case, 'FAIL: '+', '.join(errors) if errors else 'PASS', flush=True)
(out/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
raise SystemExit(1 if any(row['errors'] for row in rows) else 0)
