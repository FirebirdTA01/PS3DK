#!/usr/bin/env python3
# Cross-platform port of embed_builtin_header.ps1.
# Reads a text header file and emits a .inc file containing its contents as a
# C++ raw string literal assigned to `kBuiltinHeaderVerbatim`.
#
# Usage: embed_builtin_header.py <input_header.h> <output.inc>

import os
import sys
import uuid
from datetime import datetime, timezone


def pick_delimiter(body: str) -> str:
    for candidate in ("__VSHDR__", "__VSHDR_ALT__"):
        if f'){candidate}"' not in body:
            return candidate
    return "VSHDR_" + uuid.uuid4().hex


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} <input_header.h> <output.inc>", file=sys.stderr)
        return 2

    input_path = argv[1]
    output_path = argv[2]

    if not os.path.isfile(input_path):
        print(f"Input header not found: {input_path}", file=sys.stderr)
        return 1

    # Up-to-date check: skip regeneration if output is newer than input
    if os.path.isfile(output_path):
        if os.path.getmtime(output_path) >= os.path.getmtime(input_path):
            print(f"Embedded header up-to-date: {output_path}")
            return 0

    with open(input_path, "r", encoding="utf-8", newline="") as f:
        raw = f.read()
    # Normalize CRLF to LF (matches the PowerShell behavior)
    raw = raw.replace("\r", "")

    delim = pick_delimiter(raw)
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")
    source_name = os.path.basename(input_path)

    content = (
        f"// AUTO-GENERATED. DO NOT EDIT.\n"
        f"// Source: {source_name}\n"
        f"// Generated: {timestamp}\n"
        f"\n"
        f"static constexpr char kBuiltinHeaderVerbatim[] = R\"{delim}(\n"
        f"{raw}\n"
        f"){delim}\";"
    )

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    # Write with LF line endings regardless of platform; matches the original
    # UTF-8 -NoNewline output from PowerShell (no trailing newline).
    with open(output_path, "w", encoding="utf-8", newline="") as f:
        f.write(content)

    print(f"Embedded header generated: {output_path} (length={len(raw)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
