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

The exact 8-byte-entry/+4/lwz low-word shape is counted separately. Object-only
mode cannot distinguish a legitimate low-word read from a bad remap landing
at +4 with the same shape. The linked checker is the gate for that distinction.
"""

import argparse
import importlib.util
import os
import struct
import subprocess
import sys

# Load the shared parser module by its path next to this file: python -I
# does not put the script directory on sys.path.
_spec = importlib.util.spec_from_file_location(
    'ppc64_elf_parse', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ppc64_elf_parse.py'))
_parse = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_parse)

SUPPORTED_TOC_ENTRY_RELOCS = _parse.SUPPORTED_TOC_ENTRY_RELOCS
SUPPORTED_TOC_ACCESS_RELOCS = _parse.SUPPORTED_TOC_ACCESS_RELOCS
find_tool = _parse.find_tool
inspect_ar_container = _parse.inspect_ar_container
validate_file_format = _parse.validate_file_format
split_readelf_by_member = _parse.split_readelf_by_member
parse_sections_pass = _parse.parse_sections_pass
parse_symbols_pass = _parse.parse_symbols_pass
parse_relocations_pass = _parse.parse_relocations_pass


def object_code_sections(data, sections, needed):
    """Bound instruction reads using ELF sh_info, never inferred section names.

    Cross-check relevant binary section identities against the producer inventory. Archive
    payloads are supplied in container order, including duplicate member names.
    Unsupported extended section numbering is refused rather than guessed.
    """
    if not needed:
        return {}
    if len(data) < 64:
        raise ValueError('Truncated ELF header for instruction inspection')
    shoff = struct.unpack_from('>Q', data, 40)[0]
    entsize, count, names_index = struct.unpack_from('>HHH', data, 58)
    if entsize != 64 or count != len(sections) or not 0 < names_index < count:
        raise ValueError('Unsupported or contradictory binary section inventory')
    if shoff + count * 64 > len(data):
        raise ValueError('Truncated binary section headers')
    headers = [struct.unpack_from('>IIQQQQIIQQ', data, shoff + i * 64) for i in range(count)]

    def payload(h):
        off, size = h[4:6]
        if off + size > len(data):
            raise ValueError('Truncated binary section payload')
        return data[off:off + size]

    names = payload(headers[names_index])
    labels = []
    for i, h in enumerate(headers):
        end = names.find(b'\0', h[0])
        if end < 0:
            raise ValueError('Invalid binary section name')
        name = names[h[0]:end].decode('ascii')
        if h[5] != sections[i]['size'] or h[9] != sections[i]['es']:
            raise ValueError('Binary / producer section inventory mismatch')
        if h[1] != 8:  # SHT_NOBITS has no file payload
            payload(h)
        labels.append(name)
    toc_indices = [i for i, name in enumerate(labels) if name == '.toc']
    toc_rela_indices = [i for i, name in enumerate(labels) if name == '.rela.toc']
    if len(toc_indices) != 1 or len(toc_rela_indices) != 1:
        raise ValueError('Ambiguous binary TOC section identity')
    toc_rela = headers[toc_rela_indices[0]]
    if toc_rela[1] != 4 or toc_rela[7] != toc_indices[0]:
        raise ValueError('.rela.toc does not relocate the .toc section')
    if sum(h[1] == 4 and h[7] == toc_indices[0] and h[5] != 0 for h in headers) != 1:
        raise ValueError('Multiple relocation sections target .toc')
    result = {}
    for i, h in enumerate(headers):
        if sections[i]['name'] in needed:
            if h[1] != 4 or labels[i] != sections[i]['name']:
                raise ValueError('Binary / producer relocation section mismatch')
            if h[7] >= count or h[6] >= count:
                raise ValueError('Invalid relocation section link/info')
            target = headers[h[7]]
            if sum(other[1] == 4 and other[7] == h[7] and other[5] != 0 for other in headers) != 1:
                raise ValueError('Multiple relocation sections target recognition code')
            if labels[h[7]] != sections[h[7]]['name']:
                raise ValueError('Binary / producer code section mismatch')
            if target[1] == 1 and target[2] & 4 and not target[2] & 0x800:
                if labels[i] in result:
                    raise ValueError('Ambiguous relocation section name')
                result[labels[i]] = payload(target)
    return result


def gap_writes(word):
    """Conservative GPR write set; None terminates a linear register check.

    This deliberately small whitelist covers the measured compiler sequences.
    Branches, update forms, privileged/unknown opcodes are never skipped.
    """
    op, rt, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    if op in (14, 15, 32):  # addi, addis, lwz
        return {rt}
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29):  # rotate/logical immediate
        return {ra}
    if op == 31 and (word >> 1) & 1023 == 444:  # or / mr
        return {ra}
    if op in (36, 38, 44, 52, 54):  # non-updating stw/stb/sth/stfs/stfd
        return set()
    if op == 62 and word & 3 == 0:  # std, not stdu
        return set()
    if op in (59, 63) and (word >> 1) & 1023 == 20:
        return set()  # fsubs / fsub (no GPR writes)
    return None


def instruction_relocations(relocs, pos):
    """Relocations whose write spans intersect this instruction; None if unknown.

    The narrow footprint table is intentional. Unknown relocation widths cannot
    prove that the instruction bytes survive linking, even at another offset.
    """
    result = []
    for rel in relocs:
        rtype = rel['rtype']
        if rtype in SUPPORTED_TOC_ACCESS_RELOCS:
            width = 2
        elif rtype in ('R_PPC64_ADDR64', 'R_PPC64_UADDR64', 'R_PPC64_REL64', 'R_PPC64_TOC'):
            width = 8
        elif rtype in ('R_PPC64_ADDR32', 'R_PPC64_UADDR32', 'R_PPC64_REL32', 'R_PPC64_REL24'):
            width = 4
        else:
            return None
        if rel['offset'] < pos + 4 and pos < rel['offset'] + width:
            result.append(rel)
    return result


def recognize_low_words(violations, relocations, code_sections):
    """Return admitted violation indices and semantic access count."""
    admitted = set()
    accesses = 0
    by_site = {}
    for idx, v in enumerate(violations):
        by_site[(v['section'], v['offset'])] = idx
    for idx, v in enumerate(violations):
        if v['entry_size'] != 8 or v['delta'] != 4:
            continue
        code = code_sections.get(v['section'], b'')
        offset = v['offset']
        if offset % 4 != 2 or offset < 2 or offset + 2 > len(code):
            continue
        rels = relocations[v['section']]
        touching = instruction_relocations(rels, offset - 2)
        if touching is None or len(touching) != 1 or touching[0]['offset'] != offset:
            continue
        word = int.from_bytes(code[offset - 2:offset + 2], 'big')
        if word >> 26 != 32:  # exactly lwz, not lwa, ld, lwzu, etc.
            continue
        base = (word >> 16) & 31
        if v['rtype'] == 'R_PPC64_TOC16' and base == 2:
            admitted.add(idx)
            accesses += 1
        elif v['rtype'] == 'R_PPC64_TOC16_LO' and base != 0:
            for pos in range(offset - 6, -1, -4):
                previous = int.from_bytes(code[pos:pos + 4], 'big')
                site = pos + 2
                hi_idx = by_site.get((v['section'], site))
                here = instruction_relocations(rels, pos)
                if here is None:
                    break
                if (hi_idx is not None and hi_idx not in admitted and len(here) == 1
                        and here[0]['offset'] == site
                        and previous >> 26 == 15
                        and (previous >> 21) & 31 == base
                        and (previous >> 16) & 31 == 2):
                    hi = violations[hi_idx]
                    if (hi['rtype'] == 'R_PPC64_TOC16_HA'
                            and hi['entry_size'] == 8 and hi['delta'] == 4
                            and hi['target_addend'] == v['target_addend']):
                        admitted.update((idx, hi_idx))
                        accesses += 1
                    break
                # Any relocation capable of changing more than an immediate
                # field defeats our instruction decoding.
                if any(r['offset'] != site or r['rtype'] not in SUPPORTED_TOC_ACCESS_RELOCS for r in here):
                    break
                writes = gap_writes(previous)
                if writes is None or base in writes:
                    break
    return admitted, accesses


def audit_member(member_name, secs, symbols_by_idx, toc_sec_num, object_data=None, sections=None):
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
        'low_word_accesses': 0,
        'low_word_sites': 0,
    }

    toc_entries = []
    toc_size = sections[int(toc_sec_num)]['size'] if sections is not None and toc_sec_num else None
    for rel in secs.get('.rela.toc', []):
        rtype = rel['rtype']
        if rtype not in SUPPORTED_TOC_ENTRY_RELOCS:
            errors.append(f"{member_name}: Unknown relocation type in .rela.toc: {rtype} at offset 0x{rel['offset']:x}")
            continue
        size = SUPPORTED_TOC_ENTRY_RELOCS[rtype]
        if toc_size is not None and rel['offset'] + size > toc_size:
            errors.append(f"{member_name}: TOC entry outside .toc bounds at 0x{rel['offset']:x}")
            continue
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

            if toc_size is not None and not 0 <= effective_target < toc_size:
                errors.append(f"{member_name}: TOC reference outside .toc bounds: {sec_name} + 0x{rel['offset']:x} targets 0x{effective_target:x} (size 0x{toc_size:x})")
                continue

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

    # Ambiguous overlapping entries cannot establish an eight-byte slot.
    ordered = sorted(toc_entries, key=lambda e: e['start'])
    overlap = any(a['end'] > b['start'] for a, b in zip(ordered, ordered[1:]))
    if overlap:
        errors.append(f"{member_name}: Overlapping .toc entries")
    if object_data is not None and not errors:
        needed = {v['section'] for v in violations if v['entry_size'] == 8 and v['delta'] == 4}
        try:
            code_sections = object_code_sections(object_data, sections, needed)
        except (ValueError, struct.error) as ex:
            errors.append(f"{member_name}: {ex}")
        else:
            admitted, accesses = recognize_low_words(violations, secs, code_sections)
            stats['low_word_accesses'] = accesses
            stats['low_word_sites'] = len(admitted)
            violations = [v for i, v in enumerate(violations) if i not in admitted]
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
        description="Audit tool for PowerPC 64 TOC mid-entry relocations (Capability a).",
        epilog="Recognizes the 8-byte-entry/+4/lwz low-word shape via matched TOC16_HA/LO registers or TOC16 on r2. Object-only mode cannot distinguish a legitimate +4 low-word read from a bad remap landing at +4 with the same shape; the linked checker is the gate for that distinction."
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
    total_low_word_accesses = 0
    total_low_word_sites = 0
    archive_count = 0
    object_count = 0

    if not args.quiet:
        print("TOC Mid-Entry Relocation Audit (Capability a)")
        print(f"Readelf: {readelf_bin}")
        print(f"Target count: {total_files}\n")

    archive_summary = []

    for target in targets:
        member_bytes = []
        fmt, member_inventory, fmt_err = validate_file_format(target, member_bytes)
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
        mem_S = split_readelf_by_member(res_S.stdout, target, is_archive=is_archive)
        mem_s = split_readelf_by_member(res_s.stdout, target, is_archive=is_archive)
        mem_r = split_readelf_by_member(res_r.stdout, target, is_archive=is_archive)

        # 1. Positive member accounting: must match expected member inventory
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

        # 2. Positive member identity and sequential order check
        member_mismatch = False
        for idx in range(expected_members):
            name_S = mem_S[idx][0]
            name_s = mem_s[idx][0]
            name_r = mem_r[idx][0]
            exp_name = member_inventory[idx]
            if name_S != exp_name or name_s != exp_name or name_r != exp_name:
                print(f"[ERROR] Member identity or order mismatch at index {idx} in {target}: container={exp_name}, S={name_S}, s={name_s}, r={name_r}", file=sys.stderr)
                total_errors += 1
                member_mismatch = True
                break

        if member_mismatch:
            if is_archive:
                archive_summary.append({
                    'path': target,
                    'name': os.path.basename(target),
                    'dir': os.path.basename(os.path.dirname(target)),
                    'members': expected_members,
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
            viols, m_errs, stats = audit_member(member_name, parsed_relocs, symbols_by_idx, toc_sec_num, member_bytes[idx], s_secs)
            total_toc_entries += stats['toc_entries_count']
            total_toc_relocs += stats['toc_relocs_examined']
            total_low_word_accesses += stats['low_word_accesses']
            total_low_word_sites += stats['low_word_sites']

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
        print(f"Recognized low-word accesses: {total_low_word_accesses}")
        print(f"Recognized low-word relocation sites: {total_low_word_sites}")
        if total_errors > 0:
            print(f"Inspection / producer errors: {total_errors}")

    if total_violations > 0 or total_errors > 0:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
