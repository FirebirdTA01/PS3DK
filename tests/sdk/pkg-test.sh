#!/usr/bin/env bash
# Automated test for pkg (tools/sfo-pkg/pkg.c).
#
# Covers:
#   Row (a): Payload path over 512 characters packages successfully and
#            pkg --list displays the full filename without truncation
#            (proves Linux POSIX >512 fix in CI; on Windows host proves
#            >260 Win32 extended path fix).
#   Row (b): Unreadable/missing payload file causes pkg to exit NON-ZERO
#            with no output package file created (fixes silent exit 0 defect).
#   Row (c): Control: ordinary short-path package is byte-identical to
#            current tool output (SHA-256 fixture match and unpack verification).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"

failures=0

fail() {
    printf 'pkg-test: FAIL: %s\n' "$*" >&2
    exit 1
}

row_fail() {
    printf 'pkg-test: FAIL [%s]: %s\n' "$1" "$2" >&2
    failures=$((failures + 1))
}

note() {
    printf 'pkg-test: %s\n' "$*"
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Resolve or build the pkg binary
pkg="${1:-${PKG_BIN:-}}"
if [ -z "$pkg" ]; then
    CC="${CC:-gcc}"
    if ! command -v "$CC" >/dev/null 2>&1; then
        CC="cc"
    fi
    note "building pkg with $CC"
    "$CC" -O2 -Wall -Wextra \
        "$repo_root/tools/sfo-pkg/pkg.c" \
        "$repo_root/tools/sfo-pkg/sha1.c" \
        -o "$tmp/pkg"
    pkg="$tmp/pkg"
fi

[ -x "$pkg" ] || fail "pkg executable not found or not executable: $pkg"

# ---------------------------------------------------------------------------
# Row (a): Payload path > 512 characters (e.g. ~580 chars)
# ---------------------------------------------------------------------------
note "Row (a): testing long payload path (>512 chars)..."
row_a_ok=1
deep_in="$tmp/in_long"
for i in $(seq 1 14); do
    deep_in="$deep_in/component_padding_40_chars_0123456789"
done
mkdir -p "$deep_in"
payload_file="$deep_in/payload_test_file.txt"
printf 'payload contents for long path test\n' > "$payload_file"

long_path_len=${#payload_file}
note "long payload file path length: $long_path_len characters"
[ "$long_path_len" -ge 550 ] || fail "test setup defect: path length $long_path_len < 550"

out_long="$tmp/out_long.pkg"
if ! "$pkg" --contentid=UP0001-TEST12345_00-0000000000000001 "$tmp/in_long" "$out_long" > "$tmp/pack_long.log" 2>&1; then
    row_fail "Row (a)" "pkg failed to pack directory with >512 character path"
    row_a_ok=0
fi

if [ "$row_a_ok" -eq 1 ] && [ ! -f "$out_long" ]; then
    row_fail "Row (a)" "long-path output package was not created"
    row_a_ok=0
fi

if [ "$row_a_ok" -eq 1 ]; then
    if ! "$pkg" --list "$out_long" > "$tmp/list_long.log" 2>&1; then
        row_fail "Row (a)" "pkg --list failed on long-path package"
        row_a_ok=0
    fi
fi

if [ "$row_a_ok" -eq 1 ]; then
    expected_rel="component_padding_40_chars_0123456789"
    for i in $(seq 2 14); do
        expected_rel="$expected_rel/component_padding_40_chars_0123456789"
    done
    expected_rel="$expected_rel/payload_test_file.txt"

    if ! grep -F "$expected_rel" "$tmp/list_long.log" >/dev/null 2>&1; then
        row_fail "Row (a)" "listing does not contain complete untruncated relative path"
        row_a_ok=0
    fi
fi

if [ "$row_a_ok" -eq 1 ]; then
    note "Row (a) PASS: >512 char path packaged and listed without truncation"
fi

# ---------------------------------------------------------------------------
# Row (b): Unreadable or missing payload file causes non-zero exit and no pkg
# ---------------------------------------------------------------------------
note "Row (b): testing unreadable/missing payload file handling..."
row_b_ok=1
unreadable_in="$tmp/in_unreadable"
mkdir -p "$unreadable_in"
touch "$unreadable_in/unreadable.txt"
chmod 000 "$unreadable_in/unreadable.txt" || true

out_unreadable="$tmp/out_unreadable.pkg"

# Test unreadable file if chmod 000 took effect (non-root on POSIX)
if ! cat "$unreadable_in/unreadable.txt" >/dev/null 2>&1; then
    rc=0
    "$pkg" --contentid=UP0001-TEST12345_00-0000000000000001 "$unreadable_in" "$out_unreadable" > "$tmp/pack_unreadable.log" 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then
        row_fail "Row (b)" "pkg exited 0 on unreadable payload file (silent success defect)"
        row_b_ok=0
    fi
    if [ -f "$out_unreadable" ]; then
        row_fail "Row (b)" "pkg created output package despite unreadable payload file"
        row_b_ok=0
    fi
    if [ "$row_b_ok" -eq 1 ]; then
        note "Row (b1) PASS: unreadable payload exited non-zero ($rc) with no output package"
    fi
else
    note "Row (b1) SKIP: runner environment permits reading chmod 000 files (e.g. running as root)"
fi

# Nonexistent directory test: must fail non-zero with no package output
rc_nonexistent=0
"$pkg" --contentid=UP0001-TEST12345_00-0000000000000001 "$tmp/nonexistent_folder" "$tmp/out_nonexistent.pkg" >/dev/null 2>&1 || rc_nonexistent=$?
if [ "$rc_nonexistent" -eq 0 ]; then
    row_fail "Row (b)" "pkg exited 0 on nonexistent input directory"
    row_b_ok=0
fi
if [ -f "$tmp/out_nonexistent.pkg" ]; then
    row_fail "Row (b)" "pkg created output package for nonexistent input directory"
    row_b_ok=0
fi
if [ "$rc_nonexistent" -ne 0 ] && [ ! -f "$tmp/out_nonexistent.pkg" ]; then
    note "Row (b2) PASS: nonexistent directory exited non-zero ($rc_nonexistent) with no output package"
fi

# ---------------------------------------------------------------------------
# Row (c): Control: ordinary short-path package matches golden fixture bytes
# ---------------------------------------------------------------------------
note "Row (c): testing short-path package control..."
row_c_ok=1
control_in="$tmp/in_control"

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON="python"
fi

"$PYTHON" - "$control_in" << 'EOF'
import struct, sys, os

def make_self(app_type, is_npdrm):
    hdr = bytearray(288)
    hdr[0:9] = b"SCE\x00\x00\x00\x00\x02\x80"
    struct.pack_into(">Q", hdr, 40, 0x60)
    struct.pack_into(">Q", hdr, 88, 0x70)
    struct.pack_into(">I", hdr, 0x6c, app_type)
    if is_npdrm:
        struct.pack_into(">IIQ", hdr, 0x70, 3, 0x90, 0)
    for i in range(0x100, 0x120):
        hdr[i] = (i & 0xff)
    return bytes(hdr)

outdir = sys.argv[1]
os.makedirs(os.path.join(outdir, "USRDIR"), exist_ok=True)
with open(os.path.join(outdir, "PARAM.SFO"), "wb") as f:
    f.write(b"test content 1\n")
with open(os.path.join(outdir, "USRDIR", "EBOOT.BIN"), "wb") as f:
    f.write(make_self(8, True))
with open(os.path.join(outdir, "USRDIR", "module.sprx"), "wb") as f:
    f.write(make_self(1, False))
EOF

out_control="$tmp/out_control.pkg"
if ! "$pkg" --contentid=UP0001-TEST12345_00-0000000000000001 "$control_in" "$out_control" > "$tmp/pack_control.log" 2>&1; then
    row_fail "Row (c)" "pkg failed to pack control directory"
    row_c_ok=0
fi

if [ "$row_c_ok" -eq 1 ] && [ ! -f "$out_control" ]; then
    row_fail "Row (c)" "control output package was not created"
    row_c_ok=0
fi

if [ "$row_c_ok" -eq 1 ]; then
    ctrl_size=$(wc -c < "$out_control" | tr -d '[:space:]')
    if [ "$ctrl_size" -ne 1232 ]; then
        row_fail "Row (c)" "control package size mismatch: got $ctrl_size bytes, expected 1232"
        row_c_ok=0
    fi
fi

if [ "$row_c_ok" -eq 1 ]; then
    ctrl_sha=$(sha256sum "$out_control" 2>/dev/null | awk '{print $1}' || shasum -a 256 "$out_control" | awk '{print $1}')
    expected_sha="bea6c74ffaee481aadcb476766b1107104519b2d1d8df5e32c0ed8756805610f"
    if [ "$ctrl_sha" != "$expected_sha" ]; then
        row_fail "Row (c)" "control package SHA-256 mismatch: got $ctrl_sha, expected $expected_sha"
        row_c_ok=0
    fi
fi

if [ "$row_c_ok" -eq 1 ]; then
    "$pkg" --list "$out_control" > "$tmp/list_control.log" 2>&1 || true
    grep -F "PARAM.SFO" "$tmp/list_control.log" >/dev/null 2>&1 || { row_fail "Row (c)" "missing PARAM.SFO in list"; row_c_ok=0; }
    grep -F "USRDIR" "$tmp/list_control.log" >/dev/null 2>&1 || { row_fail "Row (c)" "missing USRDIR in list"; row_c_ok=0; }
    grep -E "NPDRM SELF:\+[[:space:]]+304:[[:space:]]+USRDIR/EBOOT\.BIN" "$tmp/list_control.log" >/dev/null 2>&1 || { row_fail "Row (c)" "missing NPDRM SELF EBOOT.BIN"; row_c_ok=0; }
    grep -E "raw data:\+[[:space:]]+288:[[:space:]]+USRDIR/module\.sprx" "$tmp/list_control.log" >/dev/null 2>&1 || { row_fail "Row (c)" "missing raw data module.sprx"; row_c_ok=0; }
fi

if [ "$row_c_ok" -eq 1 ]; then
    extract_dir="$tmp/extract_control"
    mkdir -p "$extract_dir"
    (
        cd "$extract_dir"
        "$pkg" -x "$out_control" >/dev/null 2>&1
    )
    cmp "$control_in/PARAM.SFO" "$extract_dir/UP0001-TEST12345_00-0000000000000001/PARAM.SFO" || { row_fail "Row (c)" "extracted PARAM.SFO mismatch"; row_c_ok=0; }
    cmp "$control_in/USRDIR/module.sprx" "$extract_dir/UP0001-TEST12345_00-0000000000000001/USRDIR/module.sprx" || { row_fail "Row (c)" "extracted USRDIR/module.sprx mismatch"; row_c_ok=0; }
    if [ ! -f "$extract_dir/UP0001-TEST12345_00-0000000000000001/USRDIR/EBOOT.BIN" ]; then
        row_fail "Row (c)" "extracted EBOOT.BIN missing"
        row_c_ok=0
    else
        eboot_size=$(wc -c < "$extract_dir/UP0001-TEST12345_00-0000000000000001/USRDIR/EBOOT.BIN" | tr -d '[:space:]')
        if [ "$eboot_size" -ne 304 ]; then
            row_fail "Row (c)" "extracted EBOOT.BIN size mismatch: got $eboot_size, expected 304"
            row_c_ok=0
        fi
    fi
fi

if [ "$row_c_ok" -eq 1 ]; then
    note "Row (c) PASS: control package is byte-identical (SHA-256 $expected_sha) and extracts cleanly"
fi

# ---------------------------------------------------------------------------
# Final summary
# ---------------------------------------------------------------------------
if [ "$failures" -eq 0 ]; then
    note "ALL ROWS PASSED"
    exit 0
else
    note "$failures ROW(S) FAILED"
    exit 1
fi
