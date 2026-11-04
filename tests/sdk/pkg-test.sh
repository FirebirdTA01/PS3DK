#!/usr/bin/env bash
# Automated test for pkg (tools/sfo-pkg/pkg.c) and sfo (tools/sfo-pkg/sfo.c).
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
#   Row (d): Directory recursion depth (1 to 16 levels, SkyBox) packaging and
#            unpacking byte-for-byte under pinned 2048 KiB stack reserve with
#            mandatory non-recovering ASan/UBSan, plus synthetic stack-pressure
#            negative control (1.0 MiB frame arrays) requiring exact SIGSEGV (exit 139)
#            and validating controls for compile, limit, mutation, and exit codes.
#   Row (e): SFO heap allocation, base-parity comparison against pre-change tool,
#            failure cleanups, and injected calloc OOM handling under 2048 KiB stack reserve.
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

# Pin Linux stack limit to 2048 KiB (simulating 2 MB stack reserve)
ulimit -S -s 2048
effective_stack=$(ulimit -s)
if [ "$effective_stack" -ne 2048 ]; then
    fail "could not pin stack limit to 2048 KiB (current: $effective_stack)"
fi
note "stack limit pinned to ${effective_stack} KiB"

# Resolve compiler and verify mandatory non-recovering ASan/UBSan
CC="${CC:-gcc}"
if ! command -v "$CC" >/dev/null 2>&1; then
    CC="cc"
fi

SAN_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
if ! "$CC" $SAN_FLAGS -x c /dev/null -c -o "$tmp/san_test.o" >/dev/null 2>&1; then
    fail "required non-recovering ASan/UBSan ($SAN_FLAGS) not supported by compiler: $CC"
fi
note "required non-recovering ASan/UBSan enabled: $SAN_FLAGS"

pkg="${1:-${PKG_BIN:-}}"
if [ -z "$pkg" ]; then
    note "building pkg with $CC $SAN_FLAGS"
    "$CC" -O2 -Wall -Wextra $SAN_FLAGS \
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
    if [ "$rc" -ne 1 ]; then
        row_fail "Row (b)" "pkg exited with code $rc on unreadable payload file, expected exactly 1"
        row_b_ok=0
    fi
    if [ -e "$out_unreadable" ]; then
        row_fail "Row (b)" "pkg created output package despite unreadable payload file"
        row_b_ok=0
    fi
    if [ "$rc" -eq 1 ] && [ ! -e "$out_unreadable" ]; then
        note "Row (b1) PASS: unreadable payload exited exactly 1 with no output package"
    fi
else
    note "Row (b1) SKIP: runner environment permits reading chmod 000 files (e.g. running as root)"
fi

# Nonexistent directory test: must fail non-zero with no package output
rc_nonexistent=0
"$pkg" --contentid=UP0001-TEST12345_00-0000000000000001 "$tmp/nonexistent_folder" "$tmp/out_nonexistent.pkg" >/dev/null 2>&1 || rc_nonexistent=$?
if [ "$rc_nonexistent" -ne 1 ]; then
    row_fail "Row (b)" "pkg exited with code $rc_nonexistent on nonexistent input directory, expected exactly 1"
    row_b_ok=0
fi
if [ -e "$tmp/out_nonexistent.pkg" ]; then
    row_fail "Row (b)" "pkg created output package for nonexistent input directory"
    row_b_ok=0
fi
if [ "$rc_nonexistent" -eq 1 ] && [ ! -e "$tmp/out_nonexistent.pkg" ]; then
    note "Row (b2) PASS: nonexistent directory exited exactly 1 with no output package"
fi

# (b3) Buffered write failure test (symlink to /dev/full on POSIX)
if [ -c /dev/full ]; then
    mkdir -p "$tmp/in_full"
    printf 'full test' > "$tmp/in_full/test.txt"
    rc_full=0
    ln -sf /dev/full "$tmp/out_full.pkg"
    "$pkg" --contentid=UP0001-TEST12345_00-0000000000000001 "$tmp/in_full" "$tmp/out_full.pkg" > "$tmp/pack_full.log" 2>&1 || rc_full=$?
    if [ "$rc_full" -ne 1 ]; then
        row_fail "Row (b)" "pkg exited with code $rc_full on /dev/full write failure, expected exactly 1"
        row_b_ok=0
    fi
    if [ -e "$tmp/out_full.pkg" ] || [ -L "$tmp/out_full.pkg" ]; then
        row_fail "Row (b)" "pkg left output file/symlink after write failure"
        row_b_ok=0
    fi
    if [ "$rc_full" -eq 1 ] && [ ! -e "$tmp/out_full.pkg" ] && [ ! -L "$tmp/out_full.pkg" ]; then
        note "Row (b3) PASS: /dev/full write failure exited exactly 1 and removed output"
    fi
else
    note "Row (b3) SKIP: /dev/full not available on this host"
fi

# (b4) Stat failure after first file under ASan (--wrap=stat)
wrap_c="$tmp/wrap_stat.c"
cat > "$wrap_c" <<'EOF'
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
int __real_stat(const char *, struct stat *);
int __wrap_stat(const char *path, struct stat *st) {
    static int b_calls = 0;
    if (strstr(path, "b_stat_fail.txt") && ++b_calls == 2) {
        errno = ENOENT;
        return -1;
    }
    return __real_stat(path, st);
}
EOF
if [ -n "$SAN_FLAGS" ] && "${CC:-gcc}" -O0 $SAN_FLAGS \
    "$repo_root/tools/sfo-pkg/pkg.c" \
    "$repo_root/tools/sfo-pkg/sha1.c" \
    "$wrap_c" \
    -Wl,--wrap=stat \
    -o "$tmp/pkg_stat_fail" >/dev/null 2>&1; then
    stat_in="$tmp/in_stat_fail"
    mkdir -p "$stat_in"
    printf 'first file' > "$stat_in/a_first.txt"
    printf 'second file' > "$stat_in/b_stat_fail.txt"
    rc_stat=0
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    "$tmp/pkg_stat_fail" --contentid=UP0001-TEST12345_00-0000000000000001 "$stat_in" "$tmp/out_stat_fail.pkg" > "$tmp/stat_fail.log" 2>&1 || rc_stat=$?
    if [ "$rc_stat" -ne 1 ]; then
        row_fail "Row (b)" "pkg exited with code $rc_stat on second-file stat failure, expected exactly 1"
        row_b_ok=0
    fi
    if [ -e "$tmp/out_stat_fail.pkg" ]; then
        row_fail "Row (b)" "pkg created output package despite stat failure"
        row_b_ok=0
    fi
    if grep -q "AddressSanitizer" "$tmp/stat_fail.log"; then
        row_fail "Row (b)" "AddressSanitizer detected double-free on stat failure cleanup"
        row_b_ok=0
    fi
    if [ "$rc_stat" -eq 1 ] && [ ! -e "$tmp/out_stat_fail.pkg" ] && ! grep -q "AddressSanitizer" "$tmp/stat_fail.log"; then
        note "Row (b4) PASS: stat failure on second file exited exactly 1 with no output package and 0 ASan errors"
    fi
else
    note "Row (b4) SKIP: -Wl,--wrap=stat or ASan not supported on this host"
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
# Row (d): Directory recursion depth (1 to 16 levels, SkyBox) and synthetic stack-pressure control
# ---------------------------------------------------------------------------
note "Row (d): testing nested directory recursion depth (1-16, SkyBox) and stack frames..."
row_d_ok=1

# Test trees of depth 1, 2, 3 (reporter bisection repro), 4, 5, 8, 16
for d in 1 2 3 4 5 8 16; do
    tree_d="$tmp/depth_$d"
    curr="$tree_d"
    for i in $(seq 1 "$d"); do
        curr="$curr/dir_$i"
    done
    mkdir -p "$curr"
    printf 'depth %d payload content\n' "$d" > "$curr/payload.txt"
done

# Reporter bisection repro: USRDIR/Data/SkyBox (4 directory levels from package root)
skybox_dir="$tmp/repro_skybox/USRDIR/Data/SkyBox"
mkdir -p "$skybox_dir"
printf 'skybox texture data\n' > "$skybox_dir/skybox.bin"

# Test packing and unpacking each depth under pinned 2048 KiB stack reserve
for d in 1 2 3 4 5 8 16; do
    in_dir="$tmp/depth_$d"
    out_pkg="$tmp/out_depth_$d.pkg"
    pack_log="$tmp/pack_depth_$d.log"
    pack_err="$tmp/pack_depth_$d.err"

    if ! "$pkg" --contentid=UP0001-TEST12345_00-0000000000000001 "$in_dir" "$out_pkg" > "$pack_log" 2> "$pack_err"; then
        row_fail "Row (d)" "pkg failed to pack depth $d"
        row_d_ok=0
    elif [ ! -s "$out_pkg" ]; then
        row_fail "Row (d)" "pkg output package missing or empty for depth $d"
        row_d_ok=0
    fi

    if grep -E "Sanitizer|runtime error:" "$pack_err" >/dev/null 2>&1; then
        row_fail "Row (d)" "sanitizer diagnostic detected packing depth $d"
        row_d_ok=0
    fi

    # Unpack and verify byte-identity against original payload
    if [ "$row_d_ok" -eq 1 ]; then
        extract_d="$tmp/extract_$d"
        mkdir -p "$extract_d"
        extract_err="$tmp/extract_$d.err"
        (
            cd "$extract_d"
            "$pkg" -x "$out_pkg" >/dev/null 2> "$extract_err"
        )
        if grep -E "Sanitizer|runtime error:" "$extract_err" >/dev/null 2>&1; then
            row_fail "Row (d)" "sanitizer diagnostic detected unpacking depth $d"
            row_d_ok=0
        fi

        rel_path=""
        for i in $(seq 1 "$d"); do
            rel_path="${rel_path}dir_$i/"
        done
        rel_path="${rel_path}payload.txt"

        if ! cmp -s "$in_dir/$rel_path" "$extract_d/UP0001-TEST12345_00-0000000000000001/$rel_path"; then
            row_fail "Row (d)" "extracted payload mismatch at depth $d ($rel_path)"
            row_d_ok=0
        fi
    fi
done

# Test packing and unpacking SkyBox repro tree
if [ "$row_d_ok" -eq 1 ]; then
    out_sb="$tmp/out_skybox.pkg"
    pack_sb_err="$tmp/pack_skybox.err"
    if ! "$pkg" --contentid=UP0001-TEST12345_00-0000000000000001 "$tmp/repro_skybox" "$out_sb" >/dev/null 2> "$pack_sb_err"; then
        row_fail "Row (d)" "pkg failed to pack SkyBox repro tree"
        row_d_ok=0
    fi
    if grep -E "Sanitizer|runtime error:" "$pack_sb_err" >/dev/null 2>&1; then
        row_fail "Row (d)" "sanitizer diagnostic detected packing SkyBox repro tree"
        row_d_ok=0
    fi

    extract_sb="$tmp/extract_skybox"
    mkdir -p "$extract_sb"
    extract_sb_err="$tmp/extract_skybox.err"
    (
        cd "$extract_sb"
        "$pkg" -x "$out_sb" >/dev/null 2> "$extract_sb_err"
    )
    if grep -E "Sanitizer|runtime error:" "$extract_sb_err" >/dev/null 2>&1; then
        row_fail "Row (d)" "sanitizer diagnostic detected unpacking SkyBox repro tree"
        row_d_ok=0
    fi
    if ! cmp -s "$skybox_dir/skybox.bin" "$extract_sb/UP0001-TEST12345_00-0000000000000001/USRDIR/Data/SkyBox/skybox.bin"; then
        row_fail "Row (d)" "extracted SkyBox payload content mismatch"
        row_d_ok=0
    fi
fi

# Synthetic stack-pressure negative control:
# Injects large stack arrays (1.0 MiB per frame: 2 x 512 KiB) into collect_dir,
# reproducing stack exhaustion under the pinned 2048 KiB stack limit.
# Gate enforcement controls:
# 1. Reject missing or non-pinned stack limit.
# 2. Reject no-op / absent mutation needle via shared match-count refusal helper.
# 3. Reject failed compilation (fail gate if control fails to build).
# 4. Reject wrong exit codes (require exact 0 for flat directory, exact 139 SIGSEGV for depth 4).
check_stack_control_exit() {
    local expected="$1"
    local actual="$2"
    local context="$3"
    if [ "$actual" -ne "$expected" ]; then
        row_fail "Row (d)" "synthetic stack-pressure control $context: expected exit $expected, got $actual"
        row_d_ok=0
        return 1
    fi
    return 0
}

# Shared mutation target count validator: enforces match count and fails gate on mismatch
check_mutation_target_count() {
    local needle="$1"
    local expected="$2"
    local context="$3"
    local count
    count=$(grep -F -c "$needle" "$repo_root/tools/sfo-pkg/pkg.c" || true)
    if [ "$count" -ne "$expected" ]; then
        row_fail "Row (d)" "$context target match count mismatch (expected $expected, got $count)"
        row_d_ok=0
        return 1
    fi
    return 0
}

# Unit self-checks on check_stack_control_exit (proves classifier rejects wrong exits)
check_self_failures=0
if ( check_stack_control_exit 139 0 "self-check: wrong exit 0" ) >/dev/null 2>&1; then check_self_failures=$((check_self_failures + 1)); fi
if ( check_stack_control_exit 139 1 "self-check: wrong exit 1" ) >/dev/null 2>&1; then check_self_failures=$((check_self_failures + 1)); fi
if ( check_stack_control_exit 0 139 "self-check: wrong exit 139" ) >/dev/null 2>&1; then check_self_failures=$((check_self_failures + 1)); fi
if [ "$check_self_failures" -ne 0 ]; then
    row_fail "Row (d)" "synthetic stack-pressure control exit classifier self-checks failed ($check_self_failures missed rejections)"
    row_d_ok=0
fi

# Self-check: prove check_mutation_target_count refuses an absent needle when expecting 1 match
needle_refusal_failed=0
if ( check_mutation_target_count "NONEXISTENT_MUTATION_TARGET_NEEDLE" 1 "refusal-self-check" ) >/dev/null 2>&1; then
    needle_refusal_failed=$((needle_refusal_failed + 1))
fi
if [ "$needle_refusal_failed" -ne 0 ]; then
    row_fail "Row (d)" "mutation needle match count refusal check failed (permitted missing needle)"
    row_d_ok=0
fi

# Verify mutation needle matches exactly once in pkg.c
mutation_needle='DIR *dp = opendir(folder);'
check_mutation_target_count "$mutation_needle" 1 "synthetic stack-pressure mutation"

# Verify stack limit is pinned to 2048 KiB before executing control
ctrl_limit=$(ulimit -s)
if [ "$ctrl_limit" -ne 2048 ]; then
    row_fail "Row (d)" "stack limit missing or not pinned to 2048 KiB (current: $ctrl_limit)"
    row_d_ok=0
fi

control_c="$tmp/pkg_stack_control.c"
control_bin="$tmp/pkg_stack_control"
sed -e 's/DIR \*dp = opendir(folder);/volatile char synthetic_files_list[1024 * 512]; synthetic_files_list[0] = 1; volatile char synthetic_dirs_list[1024 * 512]; synthetic_dirs_list[0] = 1; DIR *dp = opendir(folder);/' \
    "$repo_root/tools/sfo-pkg/pkg.c" > "$control_c"

if ! "$CC" -O2 -I "$repo_root/tools/sfo-pkg" "$control_c" "$repo_root/tools/sfo-pkg/sha1.c" -o "$control_bin" > "$tmp/ctrl_build.log" 2>&1; then
    row_fail "Row (d)" "synthetic stack-pressure control compilation failed (see $tmp/ctrl_build.log)"
    row_d_ok=0
fi

if [ "$row_d_ok" -eq 1 ]; then
    # Create flat directory (1 collect_dir frame = 1.0 MiB < 2048 KiB: must succeed with exit 0)
    flat_dir="$tmp/depth_flat"
    mkdir -p "$flat_dir"
    printf 'flat file content\n' > "$flat_dir/flat.txt"

    rc_ctrl_flat=0
    { (ulimit -S -s 2048; exec "$control_bin" --contentid=UP0001-TEST12345_00-0000000000000001 "$flat_dir" "$tmp/out_ctrl_flat.pkg" >/dev/null 2>&1); } 2>/dev/null || rc_ctrl_flat=$?
    check_stack_control_exit 0 "$rc_ctrl_flat" "flat directory (depth 0)"

    # Nested depth 4 (5 collect_dir frames = 5.0 MiB > 2048 KiB: must exhaust stack and crash with exact exit 139 SIGSEGV)
    rc_ctrl_4=0
    { (ulimit -S -s 2048; exec "$control_bin" --contentid=UP0001-TEST12345_00-0000000000000001 "$tmp/depth_4" "$tmp/out_ctrl_4.pkg" >/dev/null 2>&1); } 2>/dev/null || rc_ctrl_4=$?
    check_stack_control_exit 139 "$rc_ctrl_4" "nested directory (depth 4)"

    if [ "$rc_ctrl_flat" -eq 0 ] && [ "$rc_ctrl_4" -eq 139 ]; then
        note "Row (d) synthetic stack-pressure control PASS: flat tree exited 0, depth 4 crashed with exact exit 139 (SIGSEGV under Linux 2048 KiB stack; synthetic control distinct from historical binary / pending Windows execution)"
    fi
fi

if [ "$row_d_ok" -eq 1 ]; then
    note "Row (d) PASS: nested directories 1-16 levels and SkyBox repro pack, unpack, and byte-compare cleanly under 2048 KiB stack; synthetic stack pressure confirmed"
fi

# ---------------------------------------------------------------------------
# Row (e): SFO heap allocation, golden baseline parity, error cleanups, and calloc OOM
# ---------------------------------------------------------------------------
note "Row (e): testing SFO heap allocation, golden baseline parity, error paths, and calloc OOM..."
row_e_ok=1

# Compile candidate sfo binary under pinned 2048 KiB stack with mandatory non-recovering ASan/UBSan
sfo_bin="$tmp/sfo"
if ! "$CC" -O2 -Wall -Wextra $SAN_FLAGS -Wno-stringop-truncation \
    "$repo_root/tools/sfo-pkg/sfo.c" \
    -o "$sfo_bin" > "$tmp/sfo_build.log" 2>&1; then
    row_fail "Row (e)" "candidate sfo build failed (see $tmp/sfo_build.log)"
    row_e_ok=0
fi

# Locate checked-in golden fixtures (generated once from pinned pre-change blob e7efc0bc5836)
golden_dir="$repo_root/tests/sdk/fixtures/sfo-golden"
expected_sfo="$golden_dir/fixture.sfo"
expected_list="$golden_dir/fixture_list.txt"
expected_xml="$golden_dir/fixture_exported.xml"
expected_roundtrip="$golden_dir/fixture_roundtrip.sfo"

for f in "$expected_sfo" "$expected_list" "$expected_xml" "$expected_roundtrip"; do
    if [ ! -s "$f" ]; then
        row_fail "Row (e)" "missing or empty golden baseline fixture: $f"
        row_e_ok=0
    fi
done

if [ "$row_e_ok" -eq 1 ]; then
    # Candidate tool executions (running under pinned 2048 KiB stack reserve with mandatory non-recovering ASan/UBSan):
    cand_sfo="$tmp/cand_fixture.sfo"
    cand_list_log="$tmp/cand_list.log"
    cand_exported_xml="$tmp/cand_exported.xml"
    cand_roundtrip="$tmp/cand_roundtrip.sfo"

    cand_fromxml_err="$tmp/cand_fromxml.err"
    cand_list_err="$tmp/cand_list.err"
    cand_toxml_err="$tmp/cand_toxml.err"
    cand_rt_err="$tmp/cand_rt.err"

    "$sfo_bin" --fromxml "$golden_dir/input.xml" "$cand_sfo" 2> "$cand_fromxml_err"
    "$sfo_bin" -l "$cand_sfo" > "$cand_list_log" 2> "$cand_list_err"
    "$sfo_bin" -t "$cand_sfo" "$cand_exported_xml" 2> "$cand_toxml_err"
    "$sfo_bin" --fromxml "$cand_exported_xml" "$cand_roundtrip" 2> "$cand_rt_err"

    for err in "$cand_fromxml_err" "$cand_list_err" "$cand_toxml_err" "$cand_rt_err"; do
        if grep -E "Sanitizer|runtime error:" "$err" >/dev/null 2>&1; then
            row_fail "Row (e)" "sanitizer diagnostic detected in candidate run ($err)"
            row_e_ok=0
        fi
    done

    # Assert exact byte-identity between candidate outputs (under 2048 KiB) and checked-in golden baseline
    if ! cmp -s "$expected_sfo" "$cand_sfo"; then
        row_fail "Row (e)" "candidate generated SFO binary mismatch against golden baseline fixture"
        row_e_ok=0
    fi
    if ! cmp -s "$expected_list" "$cand_list_log"; then
        row_fail "Row (e)" "candidate sfo -l output mismatch against golden baseline list"
        row_e_ok=0
    fi
    if ! cmp -s "$expected_xml" "$cand_exported_xml"; then
        row_fail "Row (e)" "candidate sfo -t XML export mismatch against golden baseline XML"
        row_e_ok=0
    fi
    if ! cmp -s "$expected_roundtrip" "$cand_roundtrip"; then
        row_fail "Row (e)" "candidate round-trip SFO mismatch against golden baseline fixture"
        row_e_ok=0
    fi
fi

# Judge helper for sfo error paths:
# Asserts exact exit status, verifies expected diagnostic, and rejects sanitizer diagnostics.
judge_sfo_invocation() {
    local expected_rc="$1"
    local actual_rc="$2"
    local expected_diag="$3"
    local err_file="$4"
    local context="$5"
    if [ "$actual_rc" -ne "$expected_rc" ]; then
        row_fail "Row (e)" "$context: expected exit $expected_rc, got $actual_rc"
        row_e_ok=0
        return 1
    fi
    if [ -n "$expected_diag" ] && ! grep -F "$expected_diag" "$err_file" >/dev/null 2>&1; then
        row_fail "Row (e)" "$context: missing expected diagnostic '$expected_diag'"
        row_e_ok=0
        return 1
    fi
    if grep -E "Sanitizer|runtime error:" "$err_file" >/dev/null 2>&1; then
        row_fail "Row (e)" "$context: sanitizer error detected"
        row_e_ok=0
        return 1
    fi
    return 0
}

# Unit control on judge_sfo_invocation: verify that matching diagnostic with a failing/mismatched exit code is rejected
ctrl_diag_err="$tmp/judge_ctrl.err"
printf 'SFO: out of memory\n' > "$ctrl_diag_err"
judge_ctrl_failed=0
if ( judge_sfo_invocation 0 1 "SFO: out of memory" "$ctrl_diag_err" "judge-control: exit 1 with expected diagnostic" ) >/dev/null 2>&1; then
    judge_ctrl_failed=$((judge_ctrl_failed + 1))
fi
if [ "$judge_ctrl_failed" -ne 0 ]; then
    row_fail "Row (e)" "judge control failed: permitted expected diagnostic with failing process status"
    row_e_ok=0
fi

# Malformed and I/O error cleanups:
# Note: sfo list_sfo / convert_to_xml / convert_from_xml return void and exit 0 from main on file read errors;
# verify exact rc 0, expected stderr diagnostics, and clean sanitizer stderr without leaks/crashes.
if [ "$row_e_ok" -eq 1 ]; then
    # 5a. Nonexistent input file (rc=0, perror diagnostic, clean sanitizer stderr)
    sfo_nonexistent_err="$tmp/sfo_nonexistent.err"
    rc_nonexistent=0
    "$sfo_bin" -l "$tmp/nonexistent.sfo" >/dev/null 2> "$sfo_nonexistent_err" || rc_nonexistent=$?
    judge_sfo_invocation 0 "$rc_nonexistent" "nonexistent.sfo" "$sfo_nonexistent_err" "nonexistent input file"

    # 5b. File too small (< 20 bytes: rc=0, 'SFO: file too small', clean sanitizer stderr)
    printf 'SHORT_FILE' > "$tmp/too_small.sfo"
    sfo_small_err="$tmp/sfo_too_small.err"
    rc_small=0
    "$sfo_bin" -l "$tmp/too_small.sfo" >/dev/null 2> "$sfo_small_err" || rc_small=$?
    judge_sfo_invocation 0 "$rc_small" "SFO: file too small" "$sfo_small_err" "too-small input file"

    # 5c. Corrupt header (>= 20 bytes, bad magic: rc=0, 'SFO: invalid header', clean sanitizer stderr)
    head -c 24 /dev/zero > "$tmp/corrupt.sfo"
    sfo_corrupt_err="$tmp/sfo_corrupt.err"
    rc_corrupt=0
    "$sfo_bin" -l "$tmp/corrupt.sfo" >/dev/null 2> "$sfo_corrupt_err" || rc_corrupt=$?
    judge_sfo_invocation 0 "$rc_corrupt" "SFO: invalid header" "$sfo_corrupt_err" "corrupt header"

    # 5d. Output file open failure (rc=0, perror diagnostic, no output file created, clean sanitizer stderr)
    sfo_outfail_err="$tmp/sfo_outfail.err"
    outfail_target="$tmp/nonexistent_dir/output.xml"
    rc_outfail=0
    "$sfo_bin" -t "$cand_sfo" "$outfail_target" >/dev/null 2> "$sfo_outfail_err" || rc_outfail=$?
    judge_sfo_invocation 0 "$rc_outfail" "nonexistent_dir" "$sfo_outfail_err" "output file open failure"
    if [ -e "$outfail_target" ]; then
        row_fail "Row (e)" "output file open failure created output file ($outfail_target)"
        row_e_ok=0
    fi
fi

# 6. Calloc OOM injection via linker wrap
cat > "$tmp/wrap_calloc.c" << 'EOF'
#include <stddef.h>
#include <stdlib.h>
void *__real_calloc(size_t nmemb, size_t size);
void *__wrap_calloc(size_t nmemb, size_t size) {
    if (nmemb == 256) { /* Injected failure on KV_MAX_PAIRS allocation */
        return NULL;
    }
    return __real_calloc(nmemb, size);
}
EOF

sfo_oom_bin="$tmp/sfo_oom"
if ! "$CC" -O2 $SAN_FLAGS -Wno-stringop-truncation \
    "$repo_root/tools/sfo-pkg/sfo.c" \
    "$tmp/wrap_calloc.c" \
    -Wl,--wrap=calloc \
    -o "$sfo_oom_bin" > "$tmp/sfo_oom_build.log" 2>&1; then
    row_fail "Row (e)" "sfo_oom compilation with wrapped calloc failed (see $tmp/sfo_oom_build.log)"
    row_e_ok=0
fi

if [ "$row_e_ok" -eq 1 ]; then
    # Test list_sfo OOM injection (rc=0, 'SFO: out of memory', clean sanitizer stderr)
    oom_list_err="$tmp/oom_list.err"
    rc_oom_list=0
    "$sfo_oom_bin" -l "$cand_sfo" >/dev/null 2> "$oom_list_err" || rc_oom_list=$?
    judge_sfo_invocation 0 "$rc_oom_list" "SFO: out of memory" "$oom_list_err" "list_sfo OOM injection"

    # Test convert_to_xml OOM injection (rc=0, 'SFO: out of memory', no output file, clean sanitizer stderr)
    oom_toxml_err="$tmp/oom_toxml.err"
    rc_oom_toxml=0
    "$sfo_oom_bin" -t "$cand_sfo" "$tmp/oom.xml" >/dev/null 2> "$oom_toxml_err" || rc_oom_toxml=$?
    judge_sfo_invocation 0 "$rc_oom_toxml" "SFO: out of memory" "$oom_toxml_err" "convert_to_xml OOM injection"
    if [ -e "$tmp/oom.xml" ]; then
        row_fail "Row (e)" "convert_to_xml created output file on OOM failure"
        row_e_ok=0
    fi

    # Test convert_from_xml OOM injection (rc=0, 'SFO: out of memory', no output file, clean sanitizer stderr)
    oom_fromxml_err="$tmp/oom_fromxml.err"
    rc_oom_fromxml=0
    "$sfo_oom_bin" --fromxml "$golden_dir/input.xml" "$tmp/oom.sfo" >/dev/null 2> "$oom_fromxml_err" || rc_oom_fromxml=$?
    judge_sfo_invocation 0 "$rc_oom_fromxml" "SFO: out of memory" "$oom_fromxml_err" "convert_from_xml OOM injection"
    if [ -e "$tmp/oom.sfo" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on OOM failure"
        row_e_ok=0
    fi
fi

if [ "$row_e_ok" -eq 1 ]; then
    note "Row (e) PASS: SFO heap allocation, golden baseline parity, CLI operations, malformed error cleanups, and calloc OOM verified cleanly with 0 sanitizer defects"
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
