#!/usr/bin/env python3
"""SPRX register_module and get_ppu_guid link and import table verification.

Builds a scratch liblv2_stub.a with nidgen from the repository YAML description into a
fresh temporary directory, compiles probe code calling sys_prx_register_module and
sys_prx_get_ppu_guid (and their sysPrx* aliases), links against the scratch archive
on both ABIs (ilp32 and lp64), and asserts that the import FNIDs are present in
.rodata.sceFNID while absent symbols (spurious and unmeasured) are absent. Includes
negative controls that assert specific failure diagnostics and verify negative link
isolation via a filtered stub archive.
"""

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def find_tool(bindir, name):
    p = Path(bindir) / name
    if p.is_file():
        return p
    if p.with_suffix('.exe').is_file():
        return p.with_suffix('.exe')
    return p


def find_nidgen(root, explicit_nidgen=None):
    if explicit_nidgen:
        p = Path(explicit_nidgen).resolve()
        if p.is_file():
            return p
        if p.with_suffix('.exe').is_file():
            return p.with_suffix('.exe')
        raise FileNotFoundError(f"explicit nidgen not found: {explicit_nidgen}")

    candidates = [
        root / 'tools/target/x86_64-pc-windows-gnu/debug/nidgen.exe',
        root / 'tools/target/release/nidgen.exe',
        root / 'tools/target/debug/nidgen.exe',
        root / 'tools/target/release/nidgen',
        root / 'tools/target/debug/nidgen',
    ]
    for cand in candidates:
        if cand.is_file():
            return cand

    which = shutil.which('nidgen')
    if which:
        return Path(which)

    return None


def get_section_data(elf_path, section_name):
    data = elf_path.read_bytes()
    assert data[:6] == b'\x7fELF\x02\x02', f'{elf_path}: expected ELF64 big-endian'
    e_shoff = struct.unpack_from('>Q', data, 40)[0]
    e_shentsize, e_shnum = struct.unpack_from('>HH', data, 58)
    e_shstrndx = struct.unpack_from('>H', data, 62)[0]
    shstr_hdr = struct.unpack_from('>IIQQQQIIQQ', data, e_shoff + e_shstrndx * e_shentsize)
    shstr_offset, shstr_size = shstr_hdr[4], shstr_hdr[5]
    shstrtab = data[shstr_offset:shstr_offset + shstr_size]

    for i in range(e_shnum):
        hdr = struct.unpack_from('>IIQQQQIIQQ', data, e_shoff + i * e_shentsize)
        sh_name = hdr[0]
        null_pos = shstrtab.find(b'\x00', sh_name)
        if null_pos != -1:
            name = shstrtab[sh_name:null_pos].decode('latin1', 'replace')
            if name == section_name:
                offset, size = hdr[4], hdr[5]
                return data[offset:offset + size]
    return None


def filter_yaml_excluding(src_path, dst_path, excluded_names):
    lines = src_path.read_text(encoding='utf-8').splitlines(keepends=True)
    out_lines = []
    skipping = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith('- name:'):
            name = stripped.split(':', 1)[1].strip()
            skipping = (name in excluded_names)
        elif line.startswith('- ') or (line and not line.startswith(' ') and not line.startswith('\t')):
            skipping = False
        if not skipping:
            out_lines.append(line)
    dst_path.write_text(''.join(out_lines), encoding='utf-8')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ps3dev', type=Path, default=os.environ.get('PS3DEV'))
    p.add_argument('--nidgen', type=Path, default=None)
    p.add_argument('--output', type=Path, default=None,
                   help='Scratch output directory (defaults to a fresh tempdir)')
    args = p.parse_args()

    if not args.ps3dev:
        print('prx-register-guid-link: SKIP (set PS3DEV or --ps3dev)')
        return 0

    ps3dev = args.ps3dev.resolve()
    cc = find_tool(ps3dev / 'ppu/bin', 'powerpc64-ps3-elf-gcc')
    if not cc.is_file():
        print(f'prx-register-guid-link: SKIP (no compiler at {cc})')
        return 0

    nidgen = find_nidgen(ROOT, args.nidgen)
    if not nidgen:
        print('prx-register-guid-link: FAIL (nidgen tool binary not found)')
        return 1

    temp_dir = None
    if args.output:
        out = args.output.resolve()
        out.mkdir(parents=True, exist_ok=True)
    else:
        temp_dir = tempfile.mkdtemp(prefix='prx-link-test-')
        out = Path(temp_dir)

    try:
        yaml_input = ROOT / 'tools/nidgen/nids/extracted/liblv2_stub.yaml'
        if not yaml_input.is_file():
            print(f'prx-register-guid-link: FAIL (missing YAML: {yaml_input})')
            return 1

        # Probe source code testing types, layout assertions, canonical calls, and alias calls
        src_path = out / 'prx_probe.c'
        src_path.write_text('''#include <stdint.h>
#include <stddef.h>
#include <sys/prx.h>
#include <sys/sys_types.h>
#include <lv2/prx.h>

#ifdef FAIL_LAYOUT
_Static_assert(sizeof(sys_prx_register_module_option_t) == 999, "intentional fail layout mismatch");
#endif

_Static_assert(sizeof(sys_prx_register_module_option_t) == 8, "sys_prx_register_module_option_t size");
_Static_assert(_Alignof(sys_prx_register_module_option_t) == 8, "sys_prx_register_module_option_t align");
_Static_assert(sizeof(sysPrxRegisterModuleOption) == 8, "sysPrxRegisterModuleOption size");
_Static_assert(_Alignof(sysPrxRegisterModuleOption) == 8, "sysPrxRegisterModuleOption align");
_Static_assert(sizeof(struct _sys_prx_register_module_option) == 8, "struct _sys_prx_register_module_option size");
_Static_assert(_Alignof(struct _sys_prx_register_module_option) == 8, "struct _sys_prx_register_module_option align");

_Static_assert(sizeof(sys_ppu_guid_info_t) == 32, "sys_ppu_guid_info_t size");
_Static_assert(_Alignof(sys_ppu_guid_info_t) == 4, "sys_ppu_guid_info_t align");
_Static_assert(sizeof(struct sys_ppu_guid_info) == 32, "struct sys_ppu_guid_info size");
_Static_assert(_Alignof(struct sys_ppu_guid_info) == 4, "struct sys_ppu_guid_info align");

int main(void) {
    volatile int ret = 0;

    /* Canonical names */
    sys_prx_register_module_option_t opt;
    opt.size = sizeof(opt);
    ret += sys_prx_register_module(&opt);
    ret += (int)sys_prx_get_ppu_guid((sys_prx_id_t)0);

    /* Alias names */
    sysPrxRegisterModuleOption opt_alias;
    opt_alias.size = sizeof(opt_alias);
    ret += sysPrxRegisterModule(&opt_alias);
    ret += (int)sysPrxGetPpuGuid((sysPrxId)0);

    return (int)ret;
}
''', encoding='utf-8')

        EXPECTED_FNIDS = {
            0x5d4d351e: 'sys_prx_register_module',
            0x6010a160: 'sys_prx_get_ppu_guid',
        }

        # Filtered YAML without sys_prx_register_module and sys_prx_get_ppu_guid
        filtered_yaml = out / 'filtered_liblv2_stub.yaml'
        filter_yaml_excluding(yaml_input, filtered_yaml, {'sys_prx_register_module', 'sys_prx_get_ppu_guid'})

        env = dict(os.environ, PS3DEV=str(ps3dev))
        abis = [('ilp32', []), ('lp64', ['-mlp64'])]

        for abi, flags in abis:
            abi_out = out / abi
            abi_out.mkdir(parents=True, exist_ok=True)

            # 1a. Build filtered scratch liblv2_stub.a without the new exports
            filtered_dir = abi_out / 'filtered'
            filtered_dir.mkdir(parents=True, exist_ok=True)
            cmd_nidgen_filtered = [
                str(nidgen),
                'archive',
                '--input', str(filtered_yaml),
                '--toolchain-bin', str(ps3dev / 'ppu/bin'),
                '--abi', abi,
                '--out-dir', str(filtered_dir),
            ]
            res = subprocess.run(cmd_nidgen_filtered, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if res.returncode != 0:
                print(f'prx-register-guid-link: FAIL {abi}: nidgen archive (filtered) failed')
                print(res.stdout + res.stderr)
                return 1

            filtered_archive = filtered_dir / 'liblv2_stub.a'
            if not filtered_archive.is_file():
                print(f'prx-register-guid-link: FAIL {abi}: expected filtered archive missing: {filtered_archive}')
                return 1

            # 1b. Build full scratch liblv2_stub.a with nidgen
            full_dir = abi_out / 'full'
            full_dir.mkdir(parents=True, exist_ok=True)
            cmd_nidgen_full = [
                str(nidgen),
                'archive',
                '--input', str(yaml_input),
                '--toolchain-bin', str(ps3dev / 'ppu/bin'),
                '--abi', abi,
                '--out-dir', str(full_dir),
            ]
            res = subprocess.run(cmd_nidgen_full, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if res.returncode != 0:
                print(f'prx-register-guid-link: FAIL {abi}: nidgen archive (full) failed')
                print(res.stdout + res.stderr)
                return 1

            full_archive = full_dir / 'liblv2_stub.a'
            if not full_archive.is_file():
                print(f'prx-register-guid-link: FAIL {abi}: expected full archive missing: {full_archive}')
                return 1

            # 2. Compile probe
            probe_obj = abi_out / 'probe.o'
            cmd_compile = [
                str(cc),
                *flags,
                '-c',
                '-O2',
                '-Wall',
                '-Wextra',
                '-Werror',
                '-I', str(ROOT / 'sdk/include'),
                str(src_path),
                '-o', str(probe_obj),
            ]
            res = subprocess.run(cmd_compile, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            if res.returncode != 0:
                print(f'prx-register-guid-link: FAIL {abi}: compile failed')
                print(res.stdout + res.stderr)
                return 1

            # 3. Negative control: compile with layout failure
            cmd_fail_compile = cmd_compile + ['-DFAIL_LAYOUT']
            res = subprocess.run(cmd_fail_compile, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            combined_compile = res.stdout + res.stderr
            if res.returncode == 0:
                print(f'prx-register-guid-link: FAIL {abi}: negative compile control unexpectedly succeeded')
                return 1
            if 'intentional fail layout mismatch' not in combined_compile:
                print(f'prx-register-guid-link: FAIL {abi}: negative compile control failed for wrong reason: {combined_compile}')
                return 1

            # 4a. Negative control: link against filtered stub archive must fail with undefined references
            probe_elf_fail1 = abi_out / 'probe_fail1.elf'
            cmd_link_fail1 = [
                str(cc),
                *flags,
                '-L', str(filtered_dir),
                str(probe_obj),
                '-o', str(probe_elf_fail1),
            ]
            res = subprocess.run(cmd_link_fail1, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            combined_link1 = res.stdout + res.stderr
            if res.returncode == 0:
                print(f'prx-register-guid-link: FAIL {abi}: negative link control 1 unexpectedly succeeded')
                return 1
            if 'undefined reference' not in combined_link1:
                print(f'prx-register-guid-link: FAIL {abi}: negative link control 1 missing undefined reference: {combined_link1}')
                return 1
            if 'sys_prx_register_module' not in combined_link1:
                print(f'prx-register-guid-link: FAIL {abi}: negative link control 1 missing sys_prx_register_module: {combined_link1}')
                return 1
            if 'sys_prx_get_ppu_guid' not in combined_link1:
                print(f'prx-register-guid-link: FAIL {abi}: negative link control 1 missing sys_prx_get_ppu_guid: {combined_link1}')
                return 1

            # 4b. Negative control: search path order independence with filtered archive first
            probe_elf_fail2 = abi_out / 'probe_fail2.elf'
            cmd_link_fail2 = [
                str(cc),
                *flags,
                '-L', str(filtered_dir),
                '-L', str(full_dir),
                str(probe_obj),
                '-o', str(probe_elf_fail2),
            ]
            res = subprocess.run(cmd_link_fail2, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            combined_link2 = res.stdout + res.stderr
            if res.returncode == 0:
                print(f'prx-register-guid-link: FAIL {abi}: negative link control 2 unexpectedly succeeded')
                return 1
            if 'undefined reference' not in combined_link2:
                print(f'prx-register-guid-link: FAIL {abi}: negative link control 2 missing undefined reference: {combined_link2}')
                return 1
            if 'sys_prx_register_module' not in combined_link2:
                print(f'prx-register-guid-link: FAIL {abi}: negative link control 2 missing sys_prx_register_module: {combined_link2}')
                return 1
            if 'sys_prx_get_ppu_guid' not in combined_link2:
                print(f'prx-register-guid-link: FAIL {abi}: negative link control 2 missing sys_prx_get_ppu_guid: {combined_link2}')
                return 1

            # 5. Link probe against full scratch liblv2_stub.a
            probe_elf = abi_out / 'probe.elf'
            cmd_link = [
                str(cc),
                *flags,
                str(probe_obj),
                str(full_archive),
                '-o', str(probe_elf),
            ]
            res = subprocess.run(cmd_link, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            if res.returncode != 0:
                print(f'prx-register-guid-link: FAIL {abi}: link against scratch archive failed')
                print(res.stdout + res.stderr)
                return 1

            # 6. Verify .rodata.sceFNID import table contains expected FNIDs
            fnid_data = get_section_data(probe_elf, '.rodata.sceFNID')
            if fnid_data is None:
                print(f'prx-register-guid-link: FAIL {abi}: .rodata.sceFNID section not found in {probe_elf}')
                return 1

            found_fnids = set(val[0] for val in struct.iter_unpack('>I', fnid_data))

            for nid, name in EXPECTED_FNIDS.items():
                if nid not in found_fnids:
                    print(f'prx-register-guid-link: FAIL {abi}: FNID 0x{nid:08x} ({name}) not found in .rodata.sceFNID')
                    return 1

            # Negative control: unexported and spurious FNIDs are absent
            if 0x7df066cf in found_fnids:
                print(f'prx-register-guid-link: FAIL {abi}: FNID 0x7df066cf (sys_prx_version, not exported) unexpectedly in .rodata.sceFNID')
                return 1
            if 0xdeadbeef in found_fnids:
                print(f'prx-register-guid-link: FAIL {abi}: spurious FNID 0xdeadbeef unexpectedly present')
                return 1

            print(f'prx-register-guid-link: ok   {abi}: compiled, isolated negative links verified, linked, 2 import FNIDs verified (0x5d4d351e, 0x6010a160), unexported/spurious absent')

        print('prx-register-guid-link: PASS')
        return 0
    finally:
        if temp_dir and os.path.exists(temp_dir):
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
