#!/usr/bin/env python3
"""
TOC Mid-Entry Relocation and Linked Slot Audit Tool

Scans relocatable objects (.o, .obj) and archives (.a) for mid-entry TOC
relocations on PowerPC 64 (PS3), and optionally verifies that linked ELF
binaries have not remapped mid-entry references onto wrong TOC slots.

Usage:
    python -I -B tools/toc-midentry-audit.py [options] <files_or_dirs...>
    python -I -B tools/toc-midentry-audit.py --elf <binary.elf> <objects...>
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

TOC_RELOC_TYPES = {
    'R_PPC64_TOC16',
    'R_PPC64_TOC16_LO',
    'R_PPC64_TOC16_HI',
    'R_PPC64_TOC16_HA',
    'R_PPC64_TOC16_DS',
    'R_PPC64_TOC16_LO_DS',
}


def find_tool(tool_name, user_override=None, env_vars=None):
    """Locate toolchain binary (readelf, objdump) across PATH, env, and SDK dirs."""
    if user_override and os.path.exists(user_override):
        return user_override

    if env_vars:
        for ev in env_vars:
            val = os.environ.get(ev)
            if val and os.path.exists(val):
                return val

    # Check PS3DK / PS3DEV standard prefixes
    for env_key in ('PS3DK', 'PS3DEV'):
        base = os.environ.get(env_key)
        if base:
            candidate = os.path.join(base, 'ppu', 'bin', f'{tool_name}.exe')
            if os.path.exists(candidate):
                return candidate
            candidate_nix = os.path.join(base, 'ppu', 'bin', tool_name)
            if os.path.exists(candidate_nix):
                return candidate_nix

    # Check known absolute paths
    known_paths = [
        rf'C:\SDKs\Sony\homebrew\PS3DK\ppu\bin\{tool_name}.exe',
        rf'C:\ps3dev\ppu\bin\{tool_name}.exe',
        rf'/usr/local/ps3dev/ppu/bin/{tool_name}',
        rf'/opt/ps3dev/ppu/bin/{tool_name}',
    ]
    for p in known_paths:
        if os.path.exists(p):
            return p

    # System PATH lookup
    found = shutil.which(tool_name)
    if found:
        return found
    found_exe = shutil.which(f'{tool_name}.exe')
    if found_exe:
        return found_exe

    # Fallback to generic name
    generic = tool_name.replace('powerpc64-ps3-elf-', '')
    found_gen = shutil.which(generic)
    if found_gen:
        return found_gen

    return tool_name


class ElfReader:
    """Helper to parse ELF sections, symbols, and contents."""

    def __init__(self, elf_path, readelf_bin):
        self.path = elf_path
        self.readelf = readelf_bin
        self.sections = []
        self.opd_sec = None
        self.symbols_by_name = {}
        self.symbols_by_addr = []
        self.toc_base = None
        self._parse_sections()
        self._parse_symbols()

    def _parse_sections(self):
        res = subprocess.run([self.readelf, '-S', '--wide', self.path],
                             capture_output=True, text=True, check=False)
        if res.returncode != 0:
            raise RuntimeError(f"readelf -S failed on {self.path}: {res.stderr.strip()}")
        for line in res.stdout.splitlines():
            m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)', line)
            if m:
                idx, name, stype, vaddr, offset, size = m.groups()
                sec = {
                    'idx': int(idx),
                    'name': name,
                    'type': stype,
                    'vaddr': int(vaddr, 16),
                    'offset': int(offset, 16),
                    'size': int(size, 16),
                }
                self.sections.append(sec)
                if name == '.opd':
                    self.opd_sec = sec

    def _parse_symbols(self):
        res = subprocess.run([self.readelf, '-s', '--wide', self.path],
                             capture_output=True, text=True, check=False)
        if res.returncode != 0:
            raise RuntimeError(f"readelf -s failed on {self.path}: {res.stderr.strip()}")
        for line in res.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 8 and parts[0].endswith(':'):
                try:
                    val = int(parts[1], 16)
                    size = int(parts[2]) if parts[2].isdigit() else 0
                    stype = parts[3]
                    ndx = parts[6]
                    name = parts[7]
                    if name == '.TOC.':
                        self.toc_base = val
                    self.symbols_by_name[name] = {
                        'val': val,
                        'size': size,
                        'type': stype,
                        'ndx': ndx,
                    }
                    self.symbols_by_addr.append((val, size, name))
                except ValueError:
                    continue
        self.symbols_by_addr.sort(key=lambda x: x[0])

    def vaddr_to_file_offset(self, vaddr):
        for sec in self.sections:
            if sec['vaddr'] <= vaddr < sec['vaddr'] + sec['size']:
                return sec['offset'] + (vaddr - sec['vaddr'])
        return None

    def lookup_symbol_at(self, vaddr):
        best = None
        for sval, ssize, sname in self.symbols_by_addr:
            if sval <= vaddr:
                best = (sval, sname)
            else:
                break
        if best:
            off = vaddr - best[0]
            return f"{best[1]}+0x{off:x}" if off != 0 else best[1]
        return f"0x{vaddr:x}"


def parse_relocations_from_output(stdout_text, default_file_path):
    """Parse output of readelf -r --wide into a dictionary of members/sections."""
    members = {}
    current_member = default_file_path
    current_sec = None

    for line in stdout_text.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith('File: '):
            current_member = line[6:].strip()
            current_sec = None
            if current_member not in members:
                members[current_member] = {}
        elif line.startswith('Relocation section '):
            m = re.search(r"Relocation section '([^']+)'", line)
            if m:
                current_sec = m.group(1)
                if current_member not in members:
                    members[current_member] = {}
                members[current_member][current_sec] = []
        elif current_sec and current_member in members and not line.startswith('Offset'):
            parts = line.split()
            if len(parts) >= 5:
                try:
                    offset = int(parts[0], 16)
                    rtype = parts[2]
                    sym = parts[4]
                    addend = 0
                    if len(parts) >= 7 and parts[5] in ('+', '-'):
                        addend = int(parts[6], 16) if parts[5] == '+' else -int(parts[6], 16)
                    members[current_member][current_sec].append((offset, rtype, sym, addend))
                except ValueError:
                    continue
    return members


def audit_object_relocs(members):
    """
    Capability (a): Scan relocations for mid-entry .toc targets.
    Returns: dict mapping member_name -> list of violation records.
    """
    violations_by_member = {}
    for member_name, secs in members.items():
        toc_entries = []
        for offset, rtype, sym, addend in secs.get('.rela.toc', []):
            size = 4 if rtype.endswith('32') else 8
            toc_entries.append({
                'start': offset,
                'end': offset + size,
                'size': size,
                'rtype': rtype,
                'sym': sym,
                'addend': addend,
            })

        if not toc_entries:
            continue

        violations = []
        for sec_name, relocs in secs.items():
            if sec_name == '.rela.toc':
                continue
            for offset, rtype, sym, addend in relocs:
                if rtype in TOC_RELOC_TYPES and sym == '.toc':
                    for entry in toc_entries:
                        if entry['start'] < addend < entry['end']:
                            violations.append({
                                'section': sec_name,
                                'offset': offset,
                                'rtype': rtype,
                                'target_addend': addend,
                                'entry_start': entry['start'],
                                'entry_end': entry['end'],
                                'entry_size': entry['size'],
                                'entry_rtype': entry['rtype'],
                                'entry_sym': entry['sym'],
                                'entry_addend': entry['addend'],
                                'delta': addend - entry['start'],
                            })
                            break
        if violations:
            violations_by_member[member_name] = violations

    return violations_by_member


def verify_linked_elf_slots(elf_reader, obj_path, readelf_bin):
    """
    Capability (b): For a linked ELF and an object file, verify that TOC
    references resolve to slots holding the expected symbol+addend.
    Returns: (list of mismatch records, error_count).
    """
    # 1. Parse object relocations
    res_r = subprocess.run([readelf_bin, '-r', '--wide', obj_path],
                           capture_output=True, text=True, check=False)
    if res_r.returncode != 0:
        print(f"[ERROR] readelf -r failed on {obj_path}: {res_r.stderr.strip()}", file=sys.stderr)
        return [], 1

    members = parse_relocations_from_output(res_r.stdout, obj_path)
    if not members:
        return [], 0

    secs = next(iter(members.values()))

    # 2. Parse object symbols and section indices
    res_s = subprocess.run([readelf_bin, '-s', '--wide', obj_path],
                           capture_output=True, text=True, check=False)
    if res_s.returncode != 0:
        print(f"[ERROR] readelf -s failed on {obj_path}: {res_s.stderr.strip()}", file=sys.stderr)
        return [], 1

    obj_sec_symbols = {}
    obj_symbols_by_sec_offset = {}
    for line in res_s.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[0].endswith(':'):
            try:
                sval = int(parts[1], 16)
                ssize = int(parts[2]) if parts[2].isdigit() else 0
                stype = parts[3]
                ndx = parts[6]
                name = parts[7]
                if stype != 'SECTION' and ndx.isdigit():
                    sec_num = int(ndx)
                    if sec_num not in obj_sec_symbols:
                        obj_sec_symbols[sec_num] = name
                    if sec_num not in obj_symbols_by_sec_offset:
                        obj_symbols_by_sec_offset[sec_num] = []
                    obj_symbols_by_sec_offset[sec_num].append((sval, ssize, name))
            except ValueError:
                continue

    res_S = subprocess.run([readelf_bin, '-S', '--wide', obj_path],
                           capture_output=True, text=True, check=False)
    if res_S.returncode != 0:
        print(f"[ERROR] readelf -S failed on {obj_path}: {res_S.stderr.strip()}", file=sys.stderr)
        return [], 1
    obj_sec_idx = {}
    for line in res_S.stdout.splitlines():
        m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)', line)
        if m:
            obj_sec_idx[m.group(2)] = int(m.group(1))

    # 3. Build TOC entry mapping
    toc_entries = []
    for offset, rtype, sym, addend in secs.get('.rela.toc', []):
        size = 4 if rtype.endswith('32') else 8
        real_sym = sym
        if sym in obj_sec_idx:
            sec_num = obj_sec_idx[sym]
            if sec_num in obj_sec_symbols:
                real_sym = obj_sec_symbols[sec_num]
        elif sym.startswith(('.bss.', '.data.', '.rodata.')):
            real_sym = sym.split('.', 2)[-1]
        toc_entries.append({
            'start': offset,
            'end': offset + size,
            'size': size,
            'rtype': rtype,
            'sym': sym,
            'real_sym': real_sym,
            'addend': addend,
        })

    mismatches = []
    with open(elf_reader.path, 'rb') as f_elf:
        for sec_name, relocs in secs.items():
            if not sec_name.startswith('.rela.text'):
                continue
            func_sec = sec_name[5:]  # strip .rela
            func_name = func_sec[6:] if func_sec.startswith('.text.') else None

            # Fallback for plain .text section
            func_sym_offset = 0
            if not func_name and func_sec in obj_sec_idx:
                sec_num = obj_sec_idx[func_sec]
                sym_list = obj_symbols_by_sec_offset.get(sec_num, [])
                if sym_list:
                    # Default to first function symbol
                    func_name = sym_list[0][2]
                    func_sym_offset = sym_list[0][0]

            if not func_name:
                continue

            # Look up function in ELF
            func_entry = elf_reader.symbols_by_name.get(func_name)
            if not func_entry:
                # Try dot-prefixed symbol
                func_entry = elf_reader.symbols_by_name.get(f".{func_name}")

            if not func_entry:
                continue

            # Resolve function code entry point and TOC base
            func_code_addr = None
            func_toc = elf_reader.toc_base

            # If symbol points into .opd
            if elf_reader.opd_sec and elf_reader.opd_sec['vaddr'] <= func_entry['val'] < elf_reader.opd_sec['vaddr'] + elf_reader.opd_sec['size']:
                opd_off = elf_reader.vaddr_to_file_offset(func_entry['val'])
                if opd_off is not None:
                    f_elf.seek(opd_off)
                    func_code_addr = int.from_bytes(f_elf.read(4), 'big')
                    func_toc = int.from_bytes(f_elf.read(4), 'big')
            else:
                func_code_addr = func_entry['val']

            if func_code_addr is None or func_toc is None:
                continue

            for offset, rtype, sym, addend in relocs:
                if 'TOC16' not in rtype or sym != '.toc':
                    continue

                matched_entry = None
                for entry in toc_entries:
                    if entry['start'] <= addend < entry['end']:
                        matched_entry = entry
                        break

                if not matched_entry:
                    continue

                target_sym_entry = elf_reader.symbols_by_name.get(matched_entry['real_sym'])
                if not target_sym_entry:
                    continue

                expected_val = (target_sym_entry['val'] + matched_entry['addend']) & 0xffffffff

                instr_vaddr = func_code_addr + (offset - func_sym_offset & ~3)
                instr_off = elf_reader.vaddr_to_file_offset(instr_vaddr)
                if instr_off is None:
                    continue

                f_elf.seek(instr_off)
                instr_word = int.from_bytes(f_elf.read(4), 'big')
                if instr_word == 0x60000000:
                    # NOP (eliminated TOC HA instruction)
                    continue

                opcode = (instr_word >> 26) & 0x3f
                ra = (instr_word >> 16) & 0x1f
                d = instr_word & 0xffff
                if d >= 0x8000:
                    d -= 0x10000

                # Resolve slot address
                slot_vaddr = None
                if ra == 2:
                    slot_vaddr = func_toc + d
                elif instr_off >= 4:
                    # Check preceding instruction for addis ra, r2, ha
                    f_elf.seek(instr_off - 4)
                    prev_word = int.from_bytes(f_elf.read(4), 'big')
                    prev_opcode = (prev_word >> 26) & 0x3f
                    prev_rt = (prev_word >> 21) & 0x1f
                    prev_ra = (prev_word >> 16) & 0x1f
                    if prev_opcode == 15 and prev_rt == ra and prev_ra == 2:  # addis
                        prev_ha = prev_word & 0xffff
                        if prev_ha >= 0x8000:
                            prev_ha -= 0x10000
                        slot_vaddr = func_toc + (prev_ha << 16) + d

                if slot_vaddr and opcode in (32, 58, 14, 48, 50):  # lwz, ld, addi, lfs, lfd
                    slot_off = elf_reader.vaddr_to_file_offset(slot_vaddr)
                    if slot_off:
                        f_elf.seek(slot_off)
                        if opcode == 58:  # ld
                            raw_val = int.from_bytes(f_elf.read(8), 'big')
                            actual_val = raw_val & 0xffffffff
                        else:
                            actual_val = int.from_bytes(f_elf.read(4), 'big')

                        if actual_val != expected_val:
                            mismatches.append({
                                'object': obj_path,
                                'function': func_name,
                                'reloc_offset': offset,
                                'reloc_type': rtype,
                                'target_toc_offset': addend,
                                'is_mid_entry': addend > matched_entry['start'],
                                'elf_instr_vaddr': instr_vaddr,
                                'instr_word': instr_word,
                                'slot_vaddr': slot_vaddr,
                                'actual_val': actual_val,
                                'actual_sym': elf_reader.lookup_symbol_at(actual_val),
                                'expected_val': expected_val,
                                'expected_sym': elf_reader.lookup_symbol_at(expected_val),
                            })

    return mismatches, 0


def collect_targets(input_paths, recursive=True):
    """Expand input paths into individual object and archive files, deduplicated."""
    seen = set()
    files = []
    for path in input_paths:
        path = os.path.abspath(path)
        if os.path.isdir(path):
            if recursive:
                for root, dirs, fnames in os.walk(path):
                    for f in sorted(fnames):
                        if f.endswith(('.o', '.obj', '.a')):
                            full_p = os.path.join(root, f)
                            if full_p not in seen:
                                seen.add(full_p)
                                files.append(full_p)
            else:
                for f in sorted(os.listdir(path)):
                    if f.endswith(('.o', '.obj', '.a')):
                        full_p = os.path.join(path, f)
                        if os.path.isfile(full_p) and full_p not in seen:
                            seen.add(full_p)
                            files.append(full_p)
        elif os.path.isfile(path):
            if path not in seen:
                seen.add(path)
                files.append(path)
        else:
            print(f"Warning: path not found: {path}", file=sys.stderr)
    return files


def main():
    parser = argparse.ArgumentParser(
        description="Audit tool for PowerPC 64 TOC mid-entry relocations and linked slot integrity."
    )
    parser.add_argument('inputs', nargs='+', help="Object files (.o, .obj), archives (.a), or directories to scan.")
    parser.add_argument('--elf', help="Linked ELF executable to verify TOC slot values against (Capability b).")
    parser.add_argument('--readelf', help="Path to powerpc64-ps3-elf-readelf executable.")
    parser.add_argument('--objdump', help="Path to powerpc64-ps3-elf-objdump executable.")
    parser.add_argument('--no-recurse', action='store_true', help="Do not recursively search subdirectories.")
    parser.add_argument('--summary', action='store_true', help="Print summary table of scanned archives and files.")
    parser.add_argument('-q', '--quiet', action='store_true', help="Only output errors and violations.")
    parser.add_argument('-v', '--verbose', action='store_true', help="Enable verbose diagnostics.")

    args = parser.parse_args()

    readelf_bin = find_tool('powerpc64-ps3-elf-readelf', args.readelf, ['PPU_READELF'])
    objdump_bin = find_tool('powerpc64-ps3-elf-objdump', args.objdump, ['PPU_OBJDUMP'])

    targets = collect_targets(args.inputs, recursive=not args.no_recurse)
    if not targets:
        print("No valid target files found to audit.", file=sys.stderr)
        return 1

    total_files = len(targets)
    total_violations = 0
    total_mismatches = 0
    total_errors = 0
    archive_count = 0
    object_count = 0

    if not args.quiet:
        print(f"TOC Mid-Entry Relocation Audit")
        print(f"Readelf: {readelf_bin}")
        if args.elf:
            print(f"Linked ELF: {args.elf}")
        print(f"Target count: {total_files}\n")

    elf_reader = None
    if args.elf:
        if not os.path.exists(args.elf):
            print(f"Error: ELF file not found: {args.elf}", file=sys.stderr)
            return 2
        try:
            elf_reader = ElfReader(args.elf, readelf_bin)
        except Exception as ex:
            print(f"Error initializing ELF reader: {ex}", file=sys.stderr)
            return 2

    archive_summary = []

    for target in targets:
        is_archive = target.endswith('.a')
        if is_archive:
            archive_count += 1
        else:
            object_count += 1

        # Capability (a): Relocation scan
        res_r = subprocess.run([readelf_bin, '-r', '--wide', target],
                               capture_output=True, text=True, check=False)
        if res_r.returncode != 0:
            print(f"[ERROR] Failed to inspect relocations in {target}: {res_r.stderr.strip()}", file=sys.stderr)
            total_errors += 1
            if is_archive:
                archive_summary.append({
                    'path': target,
                    'name': os.path.basename(target),
                    'dir': os.path.basename(os.path.dirname(target)),
                    'members': 0,
                    'violations': 0,
                    'error': True,
                })
            continue

        members = parse_relocations_from_output(res_r.stdout, target)
        violations_by_member = audit_object_relocs(members)

        v_count = sum(len(v) for v in violations_by_member.values())
        total_violations += v_count

        if is_archive:
            archive_summary.append({
                'path': target,
                'name': os.path.basename(target),
                'dir': os.path.basename(os.path.dirname(target)),
                'members': len(members),
                'violations': v_count,
                'error': False,
            })

        if violations_by_member:
            for member_name, viols in violations_by_member.items():
                print(f"[VIOLATION] Mid-entry TOC relocations in {member_name} ({len(viols)} occurrence{'s' if len(viols) > 1 else ''}):")
                for v in viols:
                    print(f"  {v['section']} + 0x{v['offset']:x}: {v['rtype']} targets .toc + 0x{v['target_addend']:x}")
                    print(f"    Inside entry [0x{v['entry_start']:x}, 0x{v['entry_end']:x}) (span {v['entry_size']}B, delta +0x{v['delta']:x})")
                    print(f"    Entry target: {v['entry_sym']} + 0x{v['entry_addend']:x} ({v['entry_rtype']})")

        # Capability (b): Linked slot verification (if --elf provided and not archive)
        if elf_reader and not is_archive:
            mismatches, err = verify_linked_elf_slots(elf_reader, target, readelf_bin)
            total_errors += err
            if mismatches:
                total_mismatches += len(mismatches)
                print(f"[SLOT-MISMATCH] TOC slot corruption in {target} against {os.path.basename(args.elf)} ({len(mismatches)} occurrence{'s' if len(mismatches) > 1 else ''}):")
                for m in mismatches:
                    print(f"  {m['function']} + 0x{m['reloc_offset']:x} ({m['reloc_type']} -> .toc+0x{m['target_toc_offset']:x}, mid={m['is_mid_entry']}):")
                    print(f"    ELF instruction 0x{m['elf_instr_vaddr']:x} reads slot 0x{m['slot_vaddr']:x}")
                    print(f"    Slot holds: 0x{m['actual_val']:08x} ({m['actual_sym']})")
                    print(f"    Expected:   0x{m['expected_val']:08x} ({m['expected_sym']})")

    if args.summary or (archive_count > 0 and not args.quiet):
        print("\n--- Archive Audit Summary ---")
        for item in sorted(archive_summary, key=lambda x: (x['dir'], x['name'])):
            if item.get('error'):
                status = "ERROR"
            elif item['violations'] > 0:
                status = f"FLAGGED ({item['violations']} violations)"
            else:
                status = "CLEAN"
            loc = f"{item['dir']}/{item['name']}"
            print(f"  {loc:<45} : {status:<12} ({item['members']} members)")

    if not args.quiet:
        print("\n--- Overall Audit Result ---")
        print(f"Files audited: {total_files} ({archive_count} archives, {object_count} objects)")
        print(f"Mid-entry TOC relocations (Capability a): {total_violations}")
        if elf_reader:
            print(f"Linked TOC slot mismatches (Capability b): {total_mismatches}")
        if total_errors > 0:
            print(f"Inspection errors: {total_errors}")

    if total_violations > 0 or total_mismatches > 0 or total_errors > 0:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
