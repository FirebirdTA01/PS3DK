"""
Shared readelf-output parsers for the PPU TOC tools.

Used by tools/toc-midentry-audit.py and tools/toc-linked-check.py.  It
admits only big-endian ELF64 PowerPC objects and AR archives of them, and
parses readelf -S, -s and -r (--wide) output with positive accounting:
declared counts must match parsed rows, and anything malformed, truncated
or contradictory is returned as an error rather than skipped.
"""

import os
import re
import shutil
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


def inspect_ar_container(file_path, member_bytes=None):
    """
    Inspect an AR container, validating format admission for every constituent member.
    Validates:
      - 8-byte magic '!<arch>\\n'
      - Exact 60-byte member headers with valid `\\n trailer
      - Non-negative member size
      - Exact payload presence (detects truncated payload even on metadata members)
      - 2-byte alignment padding byte presence for odd-sized members
      - No trailing garbage after the last member
      - Long-name resolution (GNU // string table)
      - Constituent members (non-metadata) must be valid 64-bit Big-Endian PowerPC ET_REL objects
    Returns: (member_inventory_list, error_message)
    """
    try:
        file_size = os.path.getsize(file_path)
        with open(file_path, 'rb') as f:
            magic = f.read(8)
            if magic != b'!<arch>\n':
                return None, f"Not a valid AR archive: bad magic {magic!r}"

            admitted_members = []
            gnu_strtab = None

            while True:
                hdr_offset = f.tell()
                if hdr_offset == file_size:
                    break
                mhdr = f.read(60)
                if not mhdr:
                    break
                if len(mhdr) < 60:
                    return None, f"Truncated AR member header at offset {hdr_offset} (got {len(mhdr)} bytes, expected 60)"

                # Header terminator must be `\n (0x60, 0x0A)
                if mhdr[58:60] != b'`\n':
                    return None, f"Invalid AR member header terminator at offset {hdr_offset}: {mhdr[58:60]!r} (expected b'`\\n')"

                raw_name = mhdr[:16].decode('ascii', errors='replace').strip()
                try:
                    msize = int(mhdr[48:58].decode('ascii', errors='replace').strip())
                except ValueError:
                    return None, f"Invalid AR member size field for '{raw_name}' at offset {hdr_offset}"

                if msize < 0:
                    return None, f"Negative AR member size ({msize}) for '{raw_name}' at offset {hdr_offset}"

                # Payload MUST be completely present in file (validates metadata too)
                mdata = f.read(msize)
                if len(mdata) != msize:
                    return None, f"Truncated AR member payload for '{raw_name}' at offset {hdr_offset}: declared {msize} bytes, got {len(mdata)} bytes"

                # 2-byte alignment padding byte check
                if msize % 2 == 1:
                    pad = f.read(1)
                    if len(pad) != 1:
                        return None, f"Missing 2-byte alignment padding for AR member '{raw_name}' at offset {hdr_offset}"

                # Metadata members
                if raw_name in ('/', '/SYM64/', '__.SYMDEF', '__.SYMDEF SORTED'):
                    continue
                if raw_name == '//':
                    gnu_strtab = mdata
                    continue

                # Resolve member name
                if raw_name.startswith('/') and raw_name[1:].isdigit():
                    off = int(raw_name[1:])
                    if gnu_strtab is None or off >= len(gnu_strtab):
                        return None, f"Invalid long name offset /{off} for member at offset {hdr_offset}"
                    # In GNU AR, entries in // are terminated by /\n or \n
                    mname = gnu_strtab[off:].split(b'/\n')[0].split(b'\n')[0].decode('ascii', errors='replace')
                else:
                    mname = raw_name.rstrip('/')

                # Constituent member must be a valid 64-bit Big-Endian PowerPC ET_REL object
                if len(mdata) < 20:
                    return None, f"Archive member '{mname}' truncated (< 20 bytes) at offset {hdr_offset}"
                if not mdata.startswith(b'\x7fELF'):
                    return None, f"Archive member '{mname}' is not an ELF object at offset {hdr_offset}"
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
                if member_bytes is not None:
                    member_bytes.append(mdata)

            if f.tell() != file_size:
                return None, f"Trailing garbage after last AR member ({file_size - f.tell()} extra bytes)"

            return admitted_members, None
    except Exception as ex:
        return None, f"Failed to read archive container: {ex}"


def validate_file_format(file_path, member_bytes=None):
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
        admitted, err = inspect_ar_container(file_path, member_bytes)
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
        if member_bytes is not None:
            with open(file_path, 'rb') as f:
                member_bytes.append(f.read())
        return 'ELF', [os.path.basename(file_path)], None

    return None, None, "Unsupported file format (only ET_REL relocatable objects and AR archives are supported; linker scripts and non-ELF files are refused)"


def split_readelf_by_member(stdout_text, default_name, is_archive=False):
    """
    Split readelf output into an ordered list of (member_display_name, member_text).
    For archives, extracts the member name from 'File: <path>(<member>)'.
    Preserves sequential member identity and handles duplicate member names cleanly.
    """
    lines = stdout_text.splitlines()
    members = []
    curr_name = None
    curr_lines = []
    has_file_header = False
    base_default = os.path.basename(default_name)

    for line in lines:
        if line.startswith('File: '):
            has_file_header = True
            if curr_name is not None:
                members.append((curr_name, '\n'.join(curr_lines)))
            full_target = line[6:].strip()
            if is_archive:
                m = re.search(r'\(([^)]+)\)$', full_target)
                curr_name = m.group(1) if m else os.path.basename(full_target)
            else:
                curr_name = os.path.basename(full_target)
            curr_lines = []
        else:
            curr_lines.append(line)

    if curr_name is not None:
        members.append((curr_name, '\n'.join(curr_lines)))
    elif not has_file_header and stdout_text.strip():
        members.append((base_default, stdout_text))

    return members


def parse_sections_pass(s_text, member_name):
    """
    Parse section headers pass (readelf -S).
    Validates:
      - Declared count of section headers
      - Unique and contiguous section indices [0..N-1]
      - Extracts sh_size and sh_entsize for all RELA sections, deriving expected_entries
      - Ensures .toc is present if .rela.toc is present
    Returns: (sections_by_nr, toc_sec_num, rela_sections_dict, errors_list)
    """
    m = re.search(r'There are (\d+) section headers, starting at offset 0x[0-9a-fA-F]+:', s_text)
    if not m:
        return None, None, None, [f"{member_name}: Malformed or unrecognized readelf -S output (missing section headers header)"]
    declared_count = int(m.group(1))

    sections_by_nr = {}
    rela_sections = {}
    toc_sec_num = None

    for line in s_text.splitlines():
        m0 = re.match(r'^\s*\[\s*0\]\s+NULL\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)', line)
        if m0:
            if 0 in sections_by_nr:
                return None, None, None, [f"{member_name}: Duplicate section header index [0]"]
            sections_by_nr[0] = {'name': '', 'type': 'NULL', 'size': 0, 'es': 0}
            continue

        sm = re.match(r'^\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)', line)
        if sm:
            nr = int(sm.group(1))
            if nr in sections_by_nr:
                return None, None, None, [f"{member_name}: Duplicate section header index [{nr}]"]
            name = sm.group(2)
            stype = sm.group(3)
            try:
                size = int(sm.group(6), 16)
                es = int(sm.group(7), 16)
            except ValueError:
                return None, None, None, [f"{member_name}: Malformed size/entsize fields for section '{name}'"]

            sections_by_nr[nr] = {'name': name, 'type': stype, 'size': size, 'es': es}
            if name == '.toc':
                toc_sec_num = str(nr)
            elif stype == 'RELA':
                if es != 24:
                    return None, None, None, [
                        f"{member_name}: Invalid RELA section '{name}' entry size: {es} (expected 24)"
                    ]
                if size % 24 != 0:
                    return None, None, None, [
                        f"{member_name}: Invalid RELA section '{name}' size: 0x{size:x} (not a multiple of 24)"
                    ]
                rela_sections[name] = {
                    'nr': nr,
                    'size': size,
                    'entsize': es,
                    'expected_entries': size // es,
                }

    if len(sections_by_nr) != declared_count:
        return None, None, None, [
            f"{member_name}: Incomplete section headers table: declared {declared_count}, parsed {len(sections_by_nr)}"
        ]

    for i in range(declared_count):
        if i not in sections_by_nr:
            return None, None, None, [
                f"{member_name}: Missing section header index [{i}] (declared {declared_count} sections)"
            ]

    if '.rela.toc' in rela_sections and toc_sec_num is None:
        return None, None, None, [
            f"{member_name}: .rela.toc is present but required .toc section record is missing from section headers"
        ]

    return sections_by_nr, toc_sec_num, rela_sections, []


def parse_symbols_pass(sym_text, member_name, sections_by_nr):
    """
    Parse symbol table pass (readelf -s).
    Validates:
      - Declared count of symtab entries
      - Unique and contiguous symbol indices [0..K-1]
      - Every symbol's numeric Ndx references a valid section in sections_by_nr
    Returns: (symbols_by_idx_dict, errors_list)
    """
    m = re.search(r"Symbol table '\.symtab' contains (\d+) entries:", sym_text)
    if not m:
        return None, [f"{member_name}: Malformed or unrecognized readelf -s output (missing symtab header)"]
    declared_count = int(m.group(1))

    symbols_by_idx = {}
    errors = []

    for line in sym_text.splitlines():
        parts = line.split()
        if len(parts) >= 7 and parts[0].endswith(':'):
            try:
                sidx = int(parts[0][:-1])
                sval = int(parts[1], 16)
                stype = parts[3]
                ndx = parts[6]
                name = parts[7] if len(parts) >= 8 else ""
            except ValueError:
                continue

            if sidx in symbols_by_idx:
                errors.append(f"{member_name}: Duplicate symbol table index [{sidx}]")
                continue

            if ndx.isdigit() and int(ndx) not in sections_by_nr:
                errors.append(f"{member_name}: Symbol '{name}' references non-existent section index {ndx}")
                continue

            symbols_by_idx[sidx] = {'val': sval, 'ndx': ndx, 'name': name, 'type': stype}

    if errors:
        return None, errors

    if len(symbols_by_idx) != declared_count:
        return None, [f"{member_name}: Symbol table declared {declared_count} entries, parsed {len(symbols_by_idx)}"]

    for i in range(declared_count):
        if i not in symbols_by_idx:
            return None, [f"{member_name}: Missing symbol table index [{i}] (declared {declared_count} entries)"]

    return symbols_by_idx, []


def parse_relocations_pass(r_text, member_name, rela_sections):
    """
    Parse relocations pass (readelf -r).
    Enforces positive coverage against rela_sections inventory from readelf -S.
    Validates:
      - Every declared RELA section must appear exactly once (duplicate declarations fail closed)
      - Declared entry count in readelf -r must match expected_entries from section header sh_size/sh_entsize
      - Parsed row count must match expected_entries exactly
      - Every row must have required fields (explicit signed addend for symbol relocations)
      - No contradictory 'There are no relocations in this file.' if RELA sections exist
      - If 0 RELA sections exist, 'There are no relocations in this file.' must be the exclusive output
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
    expected_in_sec = None
    parsed_in_sec = 0
    parsed_relocs = {}
    errors = []

    def finalize_sec():
        nonlocal curr_sec, expected_in_sec, parsed_in_sec
        if curr_sec is not None and expected_in_sec is not None:
            if parsed_in_sec != expected_in_sec:
                errors.append(
                    f"{member_name}: Relocation section '{curr_sec}' row count mismatch: section header specifies {expected_in_sec} entries (sh_size=0x{rela_sections[curr_sec]['size']:x}, sh_entsize=0x{rela_sections[curr_sec]['entsize']:x}), but {parsed_in_sec} were parsed from readelf -r"
                )
        curr_sec = None
        expected_in_sec = None
        parsed_in_sec = 0

    for line_num, raw_line in enumerate(lines, 1):
        line = raw_line.strip()
        if not line:
            continue

        if line.startswith('Relocation section '):
            finalize_sec()
            m = re.search(r"Relocation section '([^']+)' at offset 0x[0-9a-fA-F]+ contains (\d+) entr", line)
            if not m:
                errors.append(f"{member_name}: Line {line_num}: Malformed relocation section header: {line}")
                continue

            curr_sec = m.group(1)
            decl_in_r = int(m.group(2))

            # Reject duplicate relocation sections immediately
            if curr_sec in parsed_relocs:
                errors.append(f"{member_name}: Duplicate relocation section declaration in readelf -r for '{curr_sec}'")
                return None, errors

            if curr_sec not in rela_sections:
                errors.append(f"{member_name}: Relocation section '{curr_sec}' was not declared in section headers")
                continue

            exp_entries = rela_sections[curr_sec]['expected_entries']
            if decl_in_r != exp_entries:
                errors.append(
                    f"{member_name}: Relocation section '{curr_sec}' declared entry mismatch: readelf -r declares {decl_in_r} entries, but section header specifies {exp_entries} entries (sh_size=0x{rela_sections[curr_sec]['size']:x}, sh_entsize=0x{rela_sections[curr_sec]['entsize']:x})"
                )

            parsed_relocs[curr_sec] = []
            expected_in_sec = exp_entries
            parsed_in_sec = 0
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

    # Verify that every declared RELA section from readelf -S was present in readelf -r
    for sec_name, sec_info in rela_sections.items():
        if sec_name not in parsed_relocs:
            errors.append(f"{member_name}: Missing relocation section '{sec_name}' declared in section headers ({sec_info['expected_entries']} entries expected)")

    if errors:
        return None, errors
    return parsed_relocs, []
