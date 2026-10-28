#!/usr/bin/env python3
"""
TOC Mid-Entry Relocation Audit Tool

Scans relocatable PowerPC 64 (PS3) objects (.o, .obj) and archives (.a)
for mid-entry TOC relocations where a code or data reference targets
strictly inside an existing .toc entry rather than at its start.

Enforces positive coverage accounting: verifies that every input is a
strictly compliant ET_REL object or AR archive of ET_REL objects, and
verifies that readelf producer outputs match expected member, section,
and relocation inventories with zero unparsed, truncated, or contradictory lines.
Linker scripts and non-relocatable binaries are refused.
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


def inspect_ar_container(file_path):
    """
    Inspect an AR container, validating format admission for every constituent member.
    Returns: (member_inventory_list, error_message)
    """
    try:
        with open(file_path, 'rb') as f:
            magic = f.read(8)
            if magic != b'!<arch>\n':
                return None, f"Not a valid AR archive: bad magic {magic!r}"
            admitted_members = []
            while True:
                mhdr = f.read(60)
                if not mhdr:
                    break
                if len(mhdr) < 60:
                    return None, "Truncated AR member header"
                mname = mhdr[:16].decode('ascii', errors='replace').strip()
                try:
                    msize = int(mhdr[48:58].decode('ascii', errors='replace').strip())
                except ValueError:
                    return None, f"Invalid AR member size field for '{mname}'"
                mdata = f.read(msize)
                if msize % 2 == 1:
                    f.read(1)  # 2-byte alignment padding

                # Skip standard AR symbol table entries
                if mname in ('/', '//', '/SYM64/', '__.SYMDEF', '__.SYMDEF SORTED'):
                    continue

                # Constituent member must be a valid 64-bit Big-Endian PowerPC ET_REL object
                if len(mdata) < 20:
                    return None, f"Archive member '{mname}' truncated (< 20 bytes)"
                if not mdata.startswith(b'\x7fELF'):
                    return None, f"Archive member '{mname}' is not an ELF object"
                if mdata[4] != 2:
                    return None, f"Archive member '{mname}' unsupported ELF class: {mdata[4]} (expected ELFCLASS64=2)"
                if mdata[5] != 2:
                    return None, f"Archive member '{mname}' unsupported endian: {mdata[5]} (expected Big-Endian=2)"
                e_type = int.from_bytes(mdata[16:18], 'big')
                if e_type != 1:
                    return None, f"Archive member '{mname}' unsupported ELF type: {e_type} (expected ET_REL=1 relocatable object)"
                e_machine = int.from_bytes(mdata[18:20], 'big')
                if e_machine != 21:
                    return None, f"Archive member '{mname}' unsupported machine: {e_machine} (expected EM_PPC64=21)"
                admitted_members.append(mname)
            return admitted_members, None
    except Exception as ex:
        return None, f"Failed to read archive container: {ex}"


def validate_file_format(file_path):
    """
    Validate binary magic to distinguish supported relocatable ELF and AR files.
    Fails closed on executables (ET_EXEC), shared objects, unsupported architectures,
    linker scripts, and archives containing malformed or unsupported members.
    Returns: ('ELF' | 'AR' | None, member_inventory_list, error_message)
    """
    try:
        with open(file_path, 'rb') as f:
            header = f.read(64)
    except Exception as ex:
        return None, None, f"Cannot open file: {ex}"

    if len(header) < 8:
        return None, None, "File too short (< 8 bytes)"

    if header.startswith(b'!<arch>\n'):
        admitted, err = inspect_ar_container(file_path)
        if err:
            return None, None, err
        return 'AR', admitted, None

    if header.startswith(b'\x7fELF'):
        if len(header) < 20:
            return None, None, "Truncated ELF header (< 20 bytes)"
        ei_class = header[4]
        ei_data = header[5]
        if ei_class != 2:  # ELFCLASS64
            return None, None, f"Unsupported ELF class: {ei_class} (expected ELFCLASS64=2)"
        if ei_data != 2:  # ELFDATA2MSB (Big Endian)
            return None, None, f"Unsupported ELF data encoding: {ei_data} (expected Big-Endian=2)"
        e_type = int.from_bytes(header[16:18], 'big')
        if e_type != 1:  # ET_REL
            return None, None, f"Unsupported ELF type: {e_type} (expected ET_REL=1 relocatable object; executables and shared objects are refused)"
        e_machine = int.from_bytes(header[18:20], 'big')
        if e_machine != 21:  # EM_PPC64
            return None, None, f"Unsupported ELF machine: {e_machine} (expected EM_PPC64=21)"
        return 'ELF', [os.path.basename(file_path)], None

    return None, None, "Unsupported file format (only ET_REL relocatable objects and AR archives are supported; linker scripts and non-ELF files are refused)"


def split_readelf_by_member(stdout_text, default_name):
    """
    Split readelf output into an ordered list of (member_display_name, member_text).
    Preserves sequential member identity and handles duplicate member names cleanly.
    """
    lines = stdout_text.splitlines()
    members = []
    curr_name = None
    curr_lines = []
    has_file_header = False

    for line in lines:
        if line.startswith('File: '):
            has_file_header = True
            if curr_name is not None:
                members.append((curr_name, '\n'.join(curr_lines)))
            curr_name = line[6:].strip()
            curr_lines = []
        else:
            curr_lines.append(line)

    if curr_name is not None:
        members.append((curr_name, '\n'.join(curr_lines)))
    elif not has_file_header and stdout_text.strip():
        members.append((default_name, stdout_text))

    return members


def parse_sections_pass(s_text, member_name):
    """
    Parse section headers pass (readelf -S).
    Validates declared count and contiguity of section indices.
    Returns: (sections_by_nr, toc_sec_num, rela_sections_dict, errors_list)
    """
    m = re.search(r'There are (\d+) section headers, starting at offset 0x[0-9a-fA-F]+:', s_text)
    if not m:
        return None, None, None, [f"{member_name}: Malformed or unrecognized readelf -S output (missing section headers header)"]
    declared_count = int(m.group(1))

    sections_by_nr = {}
    for line in s_text.splitlines():
        sm = re.match(r'^\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)', line)
        if sm:
            nr = int(sm.group(1))
            name = sm.group(2)
            stype = sm.group(3)
            sections_by_nr[nr] = {'name': name, 'type': stype}

    if len(sections_by_nr) != declared_count:
        return None, None, None, [
            f"{member_name}: Incomplete section headers table: declared {declared_count}, parsed {len(sections_by_nr)}"
        ]

    for i in range(declared_count):
        if i not in sections_by_nr:
            return None, None, None, [
                f"{member_name}: Missing section header index [{i}] (declared {declared_count} sections)"
            ]

    toc_sec_num = None
    rela_sections = {}
    for nr, sec in sections_by_nr.items():
        if sec['name'] == '.toc':
            toc_sec_num = str(nr)
        elif sec['type'] == 'RELA':
            rela_sections[sec['name']] = nr

    if '.rela.toc' in rela_sections and toc_sec_num is None:
        return None, None, None, [
            f"{member_name}: .rela.toc is present but required .toc section record is missing from section headers"
        ]

    return sections_by_nr, toc_sec_num, rela_sections, []


def parse_symbols_pass(sym_text, member_name, sections_by_nr):
    """
    Parse symbol table pass (readelf -s).
    Validates declared count and asserts every symbol references a valid section.
    Returns: (symbols_by_idx_dict, errors_list)
    """
    m = re.search(r"Symbol table '\.symtab' contains (\d+) entries:", sym_text)
    if not m:
        return None, [f"{member_name}: Malformed or unrecognized readelf -s output (missing symtab header)"]
    declared_count = int(m.group(1))

    symbols_by_idx = {}
    for line in sym_text.splitlines():
        parts = line.split()
        if len(parts) >= 7 and parts[0].endswith(':'):
            try:
                sidx = int(parts[0][:-1])
                sval = int(parts[1], 16)
                stype = parts[3]
                ndx = parts[6]
                name = parts[7] if len(parts) >= 8 else ""
                if ndx.isdigit() and int(ndx) not in sections_by_nr:
                    return None, [f"{member_name}: Symbol '{name}' references non-existent section index {ndx}"]
                symbols_by_idx[sidx] = {'val': sval, 'ndx': ndx, 'name': name, 'type': stype}
            except ValueError:
                continue

    if len(symbols_by_idx) != declared_count:
        return None, [f"{member_name}: Symbol table declared {declared_count} entries, parsed {len(symbols_by_idx)}"]

    return symbols_by_idx, []


def parse_relocations_pass(r_text, member_name, rela_sections):
    """
    Parse relocations pass (readelf -r).
    Enforces positive coverage against rela_sections inventory from readelf -S.
    Validates declared entry counts and required fields (including addends).
    Returns: (parsed_relocs_dict, errors_list)
    """
    lines = r_text.splitlines()

    # Case 1: no relocations declared in section headers
    if len(rela_sections) == 0:
        non_empty = [l.strip() for l in lines if l.strip()]
        if len(non_empty) == 1 and "There are no relocations in this file." in non_empty[0]:
            return {}, []
        return None, [
            f"{member_name}: Unexpected relocation output for object with no declared RELA sections: {r_text[:100]}"
        ]

    # Case 2: RELA sections declared in section headers
    if any("There are no relocations in this file." in l for l in lines):
        return None, [
            f"{member_name}: Contradictory output: section headers declared {len(rela_sections)} RELA sections, but relocations output contains 'There are no relocations in this file.'"
        ]

    curr_sec = None
    declared_in_sec = None
    parsed_in_sec = 0
    parsed_relocs = {}
    errors = []

    def finalize_sec():
        nonlocal curr_sec, declared_in_sec, parsed_in_sec
        if curr_sec is not None and declared_in_sec is not None:
            if parsed_in_sec != declared_in_sec:
                errors.append(
                    f"{member_name}: Relocation section '{curr_sec}' declared {declared_in_sec} entries, but {parsed_in_sec} were parsed"
                )
        curr_sec = None
        declared_in_sec = None
        parsed_in_sec = 0

    for line_num, raw_line in enumerate(lines, 1):
        line = raw_line.strip()
        if not line:
            continue

        if line.startswith('Relocation section '):
            finalize_sec()
            m = re.search(r"Relocation section '([^']+)' at offset 0x[0-9a-fA-F]+ contains (\d+) entr", line)
            if m:
                curr_sec = m.group(1)
                declared_in_sec = int(m.group(2))
                if curr_sec not in rela_sections:
                    errors.append(f"{member_name}: Relocation section '{curr_sec}' was not declared in section headers")
                parsed_relocs[curr_sec] = []
                parsed_in_sec = 0
            else:
                errors.append(f"{member_name}: Line {line_num}: Malformed relocation section header: {line}")
            continue

        if line.startswith('Offset') and 'Info' in line and 'Type' in line:
            continue

        if curr_sec:
            parts = line.split()
            if len(parts) < 3:
                errors.append(
                    f"{member_name}: Line {line_num}: Truncated relocation entry: {line}"
                )
                continue

            try:
                offset = int(parts[0], 16)
                info = int(parts[1], 16)
                rtype = parts[2]
                sym_idx = info >> 32
            except ValueError as ex:
                errors.append(f"{member_name}: Line {line_num}: Failed to parse relocation header fields ({ex}): {line}")
                continue

            if sym_idx != 0:
                # Every RELA row with a symbol must have at least 7 fields (including explicit + <addend> or - <addend>)
                if len(parts) < 7 or parts[5] not in ('+', '-'):
                    errors.append(
                        f"{member_name}: Line {line_num}: Truncated or malformed RELA relocation entry (missing symbol/addend): {line}"
                    )
                    continue

                sym = parts[4]
                try:
                    addend_val = int(parts[6], 16)
                    addend = addend_val if parts[5] == '+' else -addend_val
                except ValueError as ex:
                    errors.append(f"{member_name}: Line {line_num}: Failed to parse addend ({ex}): {line}")
                    continue
            else:
                # Null symbol relocation (e.g. R_PPC64_TOC with sym=0)
                sym = ""
                if len(parts) == 4:
                    try:
                        addend = int(parts[3], 16)
                    except ValueError as ex:
                        errors.append(f"{member_name}: Line {line_num}: Failed to parse null-sym addend ({ex}): {line}")
                        continue
                elif len(parts) == 5 and parts[3] in ('+', '-'):
                    try:
                        addend_val = int(parts[4], 16)
                        addend = addend_val if parts[3] == '+' else -addend_val
                    except ValueError as ex:
                        errors.append(f"{member_name}: Line {line_num}: Failed to parse null-sym signed addend ({ex}): {line}")
                        continue
                else:
                    errors.append(f"{member_name}: Line {line_num}: Malformed null-sym RELA relocation entry: {line}")
                    continue

            parsed_relocs[curr_sec].append({
                'offset': offset,
                'info': info,
                'rtype': rtype,
                'sym': sym,
                'addend': addend,
                'raw': line,
            })
            parsed_in_sec += 1

    finalize_sec()

    # Verify that every declared RELA section from readelf -S was parsed in readelf -r
    for sec_name in rela_sections:
        if sec_name not in parsed_relocs:
            errors.append(f"{member_name}: Missing relocation section '{sec_name}' declared in section headers")

    if errors:
        return None, errors
    return parsed_relocs, []


def audit_member(member_name, secs, symbols_by_idx, toc_sec_num):
    """
    Audit parsed sections of an object member for mid-entry TOC relocations.
    Uses symbol table indices (ELF64_R_SYM) for unambiguous symbol resolution.
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

    for sec_name, relocs in secs.items():
        if sec_name == '.rela.toc':
            continue

        for rel in relocs:
            rtype = rel['rtype']
            if rtype not in SUPPORTED_TOC_ACCESS_RELOCS:
                continue

            sym = rel['sym']
            addend = rel['addend']
            sym_idx = rel['info'] >> 32

            effective_target = None
            if sym == '.toc':
                effective_target = addend
            elif sym_idx in symbols_by_idx:
                sinfo = symbols_by_idx[sym_idx]
                if toc_sec_num and sinfo['ndx'] == toc_sec_num:
                    effective_target = sinfo['val'] + addend
                else:
                    # Symbol is defined in another section (.rodata, .bss, .opd) or is UND
                    effective_target = None
            else:
                errors.append(
                    f"{member_name}: Unresolved TOC reference in {sec_name} + 0x{rel['offset']:x}: symbol index {sym_idx} ('{sym}') not found in symbol table"
                )
                continue

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
        fmt, member_inventory, fmt_err = validate_file_format(target)
        if fmt_err:
            print(f"[ERROR] Invalid or malformed file {target}: {fmt_err}", file=sys.stderr)
            total_errors += 1
            continue

        is_archive = (fmt == 'AR')
        if is_archive:
            archive_count += 1
        else:
            object_count += 1

        expected_members = len(member_inventory)
        # Legitimate empty archive case
        if is_archive and expected_members == 0:
            archive_summary.append({
                'path': target,
                'name': os.path.basename(target),
                'dir': os.path.basename(os.path.dirname(target)),
                'members': 0,
                'violations': 0,
                'error': False,
            })
            continue

        # Execute readelf -S (sections), -s (symbols), and -r (relocations)
        res_S = subprocess.run([readelf_bin, '-S', '--wide', target],
                               capture_output=True, text=True, check=False)
        if res_S.returncode != 0:
            print(f"[ERROR] readelf -S failed on {target} (rc={res_S.returncode}): {res_S.stderr.strip()}", file=sys.stderr)
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

        res_s = subprocess.run([readelf_bin, '-s', '--wide', target],
                               capture_output=True, text=True, check=False)
        if res_s.returncode != 0:
            print(f"[ERROR] readelf -s failed on {target} (rc={res_s.returncode}): {res_s.stderr.strip()}", file=sys.stderr)
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

        # Split outputs by constituent member
        mem_S = split_readelf_by_member(res_S.stdout, target)
        mem_s = split_readelf_by_member(res_s.stdout, target)
        mem_r = split_readelf_by_member(res_r.stdout, target)

        # Positive member accounting: must match expected member inventory
        if len(mem_S) != expected_members or len(mem_s) != expected_members or len(mem_r) != expected_members:
            print(f"[ERROR] Producer member count mismatch for {target}: expected {expected_members} from container, got S={len(mem_S)}, s={len(mem_s)}, r={len(mem_r)}", file=sys.stderr)
            total_errors += 1
            if is_archive:
                archive_summary.append({
                    'path': target,
                    'name': os.path.basename(target),
                    'dir': os.path.basename(os.path.dirname(target)),
                    'members': max(len(mem_S), len(mem_s), len(mem_r)),
                    'violations': 0,
                    'error': True,
                })
            continue

        arch_violations = 0
        arch_members = len(mem_r)
        arch_has_error = False
        total_members_examined += arch_members

        for idx, (member_name, r_text) in enumerate(mem_r):
            # Parse section headers pass
            s_secs, toc_sec_num, rela_sections, s_errs = parse_sections_pass(mem_S[idx][1], member_name)
            if s_errs:
                for se in s_errs:
                    print(f"[ERROR] {se}", file=sys.stderr)
                total_errors += len(s_errs)
                arch_has_error = True
                continue

            # Parse symbol table pass
            symbols_by_idx, sym_errs = parse_symbols_pass(mem_s[idx][1], member_name, s_secs)
            if sym_errs:
                for se in sym_errs:
                    print(f"[ERROR] {se}", file=sys.stderr)
                total_errors += len(sym_errs)
                arch_has_error = True
                continue

            # Parse relocations pass
            parsed_relocs, r_errs = parse_relocations_pass(r_text, member_name, rela_sections)
            if r_errs:
                for re_err in r_errs:
                    print(f"[ERROR] {re_err}", file=sys.stderr)
                total_errors += len(r_errs)
                arch_has_error = True
                continue

            # Audit relocations for mid-entry references
            viols, m_errs, stats = audit_member(member_name, parsed_relocs, symbols_by_idx, toc_sec_num)
            total_toc_entries += stats['toc_entries_count']
            total_toc_relocs += stats['toc_relocs_examined']

            if m_errs:
                for me in m_errs:
                    print(f"[ERROR] {me}", file=sys.stderr)
                total_errors += len(m_errs)
                arch_has_error = True

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
                'error': arch_has_error,
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
