#!/usr/bin/env python3
"""
TOC Mid-Entry Relocation Audit Tool

Scans relocatable PowerPC 64 (PS3) objects (.o, .obj) and archives (.a)
for mid-entry TOC relocations where a code or data reference targets
strictly inside an existing .toc entry rather than at its start.

Fails closed on any producer failure, unparseable relocation line,
unknown relocation width, missing input, or invalid tool override.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

# Explicit table of known relocation widths for entries in .rela.toc
SUPPORTED_TOC_ENTRY_RELOCS = {
    'R_PPC64_ADDR64': 8,
    'R_PPC64_TOC': 8,
    'R_PPC64_ADDR32': 4,
    'R_PPC64_ADDR16': 2,
    'R_PPC64_ADDR16_LO': 2,
    'R_PPC64_ADDR16_HI': 2,
    'R_PPC64_ADDR16_HA': 2,
}

# TOC-referencing relocation types
SUPPORTED_TOC_ACCESS_RELOCS = {
    'R_PPC64_TOC16',
    'R_PPC64_TOC16_LO',
    'R_PPC64_TOC16_HI',
    'R_PPC64_TOC16_HA',
    'R_PPC64_TOC16_DS',
    'R_PPC64_TOC16_LO_DS',
}


def find_tool(tool_name, user_override=None, env_vars=None):
    """Locate toolchain binary across PATH, env, and SDK dirs."""
    if user_override:
        if os.path.isfile(user_override):
            return user_override
        print(f"Error: Specified tool override not found: {user_override}", file=sys.stderr)
        return None

    if env_vars:
        for ev in env_vars:
            val = os.environ.get(ev)
            if val:
                if os.path.isfile(val):
                    return val
                print(f"Error: Tool path from environment variable {ev} not found: {val}", file=sys.stderr)
                return None

    # Check PS3DK / PS3DEV standard prefixes
    for env_key in ('PS3DK', 'PS3DEV'):
        base = os.environ.get(env_key)
        if base:
            candidate = os.path.join(base, 'ppu', 'bin', f'{tool_name}.exe')
            if os.path.isfile(candidate):
                return candidate
            candidate_nix = os.path.join(base, 'ppu', 'bin', tool_name)
            if os.path.isfile(candidate_nix):
                return candidate_nix

    # Check known absolute paths
    known_paths = [
        rf'C:\SDKs\Sony\homebrew\PS3DK\ppu\bin\{tool_name}.exe',
        rf'C:\ps3dev\ppu\bin\{tool_name}.exe',
        rf'/usr/local/ps3dev/ppu/bin/{tool_name}',
        rf'/opt/ps3dev/ppu/bin/{tool_name}',
    ]
    for p in known_paths:
        if os.path.isfile(p):
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

    print(f"Error: Toolchain binary '{tool_name}' not found on PATH or SDK dirs.", file=sys.stderr)
    return None


def validate_file_format(file_path):
    """
    Validate binary magic to distinguish ELF and AR files from malformed files.
    Returns: ('ELF' | 'AR' | None, error_message)
    """
    try:
        with open(file_path, 'rb') as f:
            header = f.read(64)
    except Exception as ex:
        return None, f"Cannot open file: {ex}"

    if len(header) < 8:
        return None, "File too short (< 8 bytes)"

    if header.startswith(b'!<arch>\n'):
        return 'AR', None

    if header.startswith(b'\x7fELF'):
        if len(header) < 20:
            return None, "Truncated ELF header (< 20 bytes)"
        ei_class = header[4]
        ei_data = header[5]
        if ei_class != 2:  # ELFCLASS64
            return None, f"Unsupported ELF class: {ei_class} (expected ELFCLASS64=2)"
        if ei_data != 2:  # ELFDATA2MSB (Big Endian)
            return None, f"Unsupported ELF data encoding: {ei_data} (expected Big-Endian=2)"
        e_type = int.from_bytes(header[16:18], 'big')
        e_machine = int.from_bytes(header[18:20], 'big')
        if e_machine != 21:  # EM_PPC64
            return None, f"Unsupported ELF machine: {e_machine} (expected EM_PPC64=21)"
        return 'ELF', None

    return None, "Not a valid ELF object or AR archive (magic unrecognized)"


def parse_relocations_output(stdout_text, default_file_path):
    """
    Parse output of readelf -r --wide into a dictionary of members/sections.
    Fails closed if any line inside a relocation section cannot be parsed.
    Returns: (members_dict, unparsed_errors)
    """
    members = {}
    current_member = default_file_path
    current_sec = None
    errors = []

    for line_num, raw_line in enumerate(stdout_text.splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue

        if line.startswith('File: '):
            current_member = line[6:].strip()
            current_sec = None
            if current_member not in members:
                members[current_member] = {}
            continue

        if line.startswith('Relocation section '):
            m = re.search(r"Relocation section '([^']+)'", line)
            if m:
                current_sec = m.group(1)
                if current_member not in members:
                    members[current_member] = {}
                members[current_member][current_sec] = []
            else:
                errors.append(f"Line {line_num}: Malformed relocation section header: {line}")
            continue

        if line.startswith('Offset') and 'Info' in line and 'Type' in line:
            # Header line of relocation table
            continue

        if current_sec and current_member in members:
            # Expected relocation table row:
            # Offset Info Type Symbol's Value Symbol's Name + Addend
            parts = line.split()
            if len(parts) >= 3:
                try:
                    offset = int(parts[0], 16)
                    info = int(parts[1], 16)
                    rtype = parts[2]
                except ValueError:
                    errors.append(f"Line {line_num}: Failed to parse offset/info in relocation line: {line}")
                    continue

                sym = ""
                addend = 0
                if len(parts) >= 5:
                    sym = parts[4]
                    if len(parts) >= 7 and parts[5] in ('+', '-'):
                        try:
                            addend_val = int(parts[6], 16)
                            addend = addend_val if parts[5] == '+' else -addend_val
                        except ValueError:
                            errors.append(f"Line {line_num}: Failed to parse addend: {line}")
                            continue

                members[current_member][current_sec].append({
                    'offset': offset,
                    'info': info,
                    'rtype': rtype,
                    'sym': sym,
                    'addend': addend,
                    'raw': line,
                })
            else:
                errors.append(f"Line {line_num}: Unexpected line under relocation section '{current_sec}': {line}")

    return members, errors


def audit_member_relocs(member_name, secs, toc_symbols=None):
    """
    Audit a single object's relocations for mid-entry TOC references.
    Returns: (violations_list, errors_list, stats_dict)
    """
    violations = []
    errors = []
    stats = {
        'toc_entries_count': 0,
        'toc_relocs_examined': 0,
        'sections_examined': len(secs),
        'has_toc': False,
    }

    toc_entries = []
    for rel in secs.get('.rela.toc', []):
        rtype = rel['rtype']
        if rtype not in SUPPORTED_TOC_ENTRY_RELOCS:
            errors.append(f"{member_name}: Unknown relocation type in .rela.toc: {rtype} at offset 0x{rel['offset']:x}")
            continue
        size = SUPPORTED_TOC_ENTRY_RELOCS[rtype]
        toc_entries.append({
            'start': rel['offset'],
            'end': rel['offset'] + size,
            'size': size,
            'rtype': rtype,
            'sym': rel['sym'],
            'addend': rel['addend'],
        })

    if toc_entries:
        stats['has_toc'] = True
        stats['toc_entries_count'] = len(toc_entries)

    toc_sym_map = toc_symbols or {}

    for sec_name, relocs in secs.items():
        if sec_name == '.rela.toc':
            continue
        for rel in relocs:
            rtype = rel['rtype']
            if rtype not in SUPPORTED_TOC_ACCESS_RELOCS:
                continue

            sym = rel['sym']
            addend = rel['addend']

            effective_target = None
            if sym == '.toc':
                effective_target = addend
            elif sym in toc_sym_map:
                effective_target = toc_sym_map[sym] + addend

            if effective_target is None:
                continue

            stats['toc_relocs_examined'] += 1

            for entry in toc_entries:
                if entry['start'] < effective_target < entry['end']:
                    violations.append({
                        'section': sec_name,
                        'offset': rel['offset'],
                        'rtype': rtype,
                        'target_addend': effective_target,
                        'entry_start': entry['start'],
                        'entry_end': entry['end'],
                        'entry_size': entry['size'],
                        'entry_rtype': entry['rtype'],
                        'entry_sym': entry['sym'],
                        'entry_addend': entry['addend'],
                        'delta': effective_target - entry['start'],
                    })
                    break

    return violations, errors, stats


def collect_targets(input_paths, recursive=True):
    """
    Expand input paths into individual object and archive files.
    Fails closed if any specified path does not exist.
    """
    seen = set()
    files = []
    errors = []

    for path_str in input_paths:
        if not os.path.exists(path_str):
            errors.append(f"Input path not found: {path_str}")
            continue

        abs_path = os.path.abspath(path_str)
        if os.path.isdir(abs_path):
            if recursive:
                for root, _, fnames in os.walk(abs_path):
                    for f in sorted(fnames):
                        if f.endswith(('.o', '.obj', '.a')):
                            full_p = os.path.join(root, f)
                            if full_p not in seen:
                                seen.add(full_p)
                                files.append(full_p)
            else:
                for f in sorted(os.listdir(abs_path)):
                    if f.endswith(('.o', '.obj', '.a')):
                        full_p = os.path.join(abs_path, f)
                        if os.path.isfile(full_p) and full_p not in seen:
                            seen.add(full_p)
                            files.append(full_p)
        elif os.path.isfile(abs_path):
            if abs_path not in seen:
                seen.add(abs_path)
                files.append(abs_path)
        else:
            errors.append(f"Input path is neither a file nor a directory: {path_str}")

    return files, errors


def parse_object_symbols(readelf_bin, target_path):
    """
    Extract symbols defined in .toc section for standalone objects.
    Returns: (toc_symbols_dict, errors)
    """
    res = subprocess.run([readelf_bin, '-s', '--wide', target_path],
                         capture_output=True, text=True, check=False)
    if res.returncode != 0:
        return {}, [f"readelf -s failed on {target_path}: {res.stderr.strip()}"]

    res_S = subprocess.run([readelf_bin, '-S', '--wide', target_path],
                           capture_output=True, text=True, check=False)
    if res_S.returncode != 0:
        return {}, [f"readelf -S failed on {target_path}: {res_S.stderr.strip()}"]

    toc_sec_num = None
    for line in res_S.stdout.splitlines():
        m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)', line)
        if m and m.group(2) == '.toc':
            toc_sec_num = m.group(1)
            break

    toc_syms = {}
    if toc_sec_num:
        for line in res.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 8 and parts[0].endswith(':'):
                try:
                    sval = int(parts[1], 16)
                    stype = parts[3]
                    ndx = parts[6]
                    name = parts[7]
                    if ndx == toc_sec_num and stype != 'SECTION':
                        toc_syms[name] = sval
                except ValueError:
                    continue

    return toc_syms, []


def main():
    parser = argparse.ArgumentParser(
        description="Audit tool for PowerPC 64 TOC mid-entry relocations (Capability a)."
    )
    parser.add_argument('inputs', nargs='+', help="Object files (.o, .obj), archives (.a), or directories to scan.")
    parser.add_argument('--readelf', help="Path to powerpc64-ps3-elf-readelf executable.")
    parser.add_argument('--no-recurse', action='store_true', help="Do not recursively search subdirectories.")
    parser.add_argument('--summary', action='store_true', help="Print summary table of scanned archives and files.")
    parser.add_argument('-q', '--quiet', action='store_true', help="Only output errors and violations.")
    parser.add_argument('-v', '--verbose', action='store_true', help="Enable verbose diagnostics.")

    args = parser.parse_args()

    # Fail closed on missing/invalid explicit --readelf
    readelf_bin = find_tool('powerpc64-ps3-elf-readelf', args.readelf, ['PPU_READELF'])
    if not readelf_bin:
        return 2

    targets, collect_errors = collect_targets(args.inputs, recursive=not args.no_recurse)
    if collect_errors:
        for err in collect_errors:
            print(f"Error: {err}", file=sys.stderr)
        return 2

    if not targets:
        print("Error: No valid target files found to audit.", file=sys.stderr)
        return 2

    total_files = len(targets)
    total_violations = 0
    total_errors = 0
    total_members_examined = 0
    total_toc_entries = 0
    total_toc_relocs = 0
    archive_count = 0
    object_count = 0

    if not args.quiet:
        print("TOC Mid-Entry Relocation Audit (Capability a)")
        print(f"Readelf: {readelf_bin}")
        print(f"Target count: {total_files}\n")

    archive_summary = []

    for target in targets:
        fmt, fmt_err = validate_file_format(target)
        if fmt_err:
            print(f"[ERROR] Invalid or malformed file {target}: {fmt_err}", file=sys.stderr)
            total_errors += 1
            continue

        is_archive = (fmt == 'AR')
        if is_archive:
            archive_count += 1
        else:
            object_count += 1

        res_r = subprocess.run([readelf_bin, '-r', '--wide', target],
                               capture_output=True, text=True, check=False)
        if res_r.returncode != 0:
            print(f"[ERROR] readelf -r failed on {target} (rc={res_r.returncode}): {res_r.stderr.strip()}", file=sys.stderr)
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

        members, parse_errs = parse_relocations_output(res_r.stdout, target)
        if parse_errs:
            for pe in parse_errs:
                print(f"[ERROR] {target}: {pe}", file=sys.stderr)
            total_errors += len(parse_errs)
            continue

        toc_symbols = {}
        if not is_archive:
            toc_symbols, sym_errs = parse_object_symbols(readelf_bin, target)
            if sym_errs:
                for se in sym_errs:
                    print(f"[ERROR] {se}", file=sys.stderr)
                total_errors += len(sym_errs)

        arch_violations = 0
        arch_members = len(members)
        total_members_examined += arch_members

        for member_name, secs in members.items():
            viols, m_errs, stats = audit_member_relocs(member_name, secs, toc_symbols)
            total_toc_entries += stats['toc_entries_count']
            total_toc_relocs += stats['toc_relocs_examined']

            if m_errs:
                for me in m_errs:
                    print(f"[ERROR] {me}", file=sys.stderr)
                total_errors += len(m_errs)

            if viols:
                v_count = len(viols)
                total_violations += v_count
                arch_violations += v_count
                print(f"[VIOLATION] Mid-entry TOC relocations in {member_name} ({v_count} occurrence{'s' if v_count > 1 else ''}):")
                for v in viols:
                    print(f"  {v['section']} + 0x{v['offset']:x}: {v['rtype']} targets .toc + 0x{v['target_addend']:x}")
                    print(f"    Inside entry [0x{v['entry_start']:x}, 0x{v['entry_end']:x}) (span {v['entry_size']}B, delta +0x{v['delta']:x})")
                    print(f"    Entry target: {v['entry_sym']} + 0x{v['entry_addend']:x} ({v['entry_rtype']})")

        if is_archive:
            archive_summary.append({
                'path': target,
                'name': os.path.basename(target),
                'dir': os.path.basename(os.path.dirname(target)),
                'members': arch_members,
                'violations': arch_violations,
                'error': False,
            })

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
        print("\n--- Overall Audit Coverage & Results ---")
        print(f"Files audited: {total_files} ({archive_count} archives, {object_count} objects)")
        print(f"Total archive members / objects inspected: {total_members_examined}")
        print(f"Total .toc entries examined: {total_toc_entries}")
        print(f"Total TOC-referencing relocations examined: {total_toc_relocs}")
        print(f"Mid-entry TOC relocations (Capability a): {total_violations}")
        if total_errors > 0:
            print(f"Inspection / producer errors: {total_errors}")

    if total_violations > 0 or total_errors > 0:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
