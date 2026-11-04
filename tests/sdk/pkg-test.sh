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
if [ "${PKG_TEST_ROW:-all}" != "e" ]; then
    if [ -z "$pkg" ]; then
        note "building pkg with $CC $SAN_FLAGS"
        "$CC" -O2 -Wall -Wextra $SAN_FLAGS \
            "$repo_root/tools/sfo-pkg/pkg.c" \
            "$repo_root/tools/sfo-pkg/sha1.c" \
            -o "$tmp/pkg"
        pkg="$tmp/pkg"
    fi

    [ -x "$pkg" ] || fail "pkg executable not found or not executable: $pkg"
fi

if [ "${PKG_TEST_ROW:-all}" != "e" ]; then
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
fi # [ "${PKG_TEST_ROW:-all}" != "e" ]

# ---------------------------------------------------------------------------
# Row (e): SFO heap allocation, golden baseline parity, error cleanups, and calloc OOM
# ---------------------------------------------------------------------------
note "Row (e): testing SFO heap allocation, golden baseline parity, error paths, and calloc OOM..."
row_e_ok=1

evidence_dir="${SFO_TEST_EVIDENCE_DIR:-}"
if [ -n "$evidence_dir" ]; then
    if [ -e "$evidence_dir" ]; then
        echo "ERROR: SFO_TEST_EVIDENCE_DIR destination already exists: $evidence_dir" >&2
        exit 1
    fi
    mkdir -p "$evidence_dir/cases"
    echo "CASE_ID | EXIT_CODE | VERDICT | DESCRIPTION" > "$evidence_dir/INDEX.txt"
    echo "--------------------------------------------------------------------------------" >> "$evidence_dir/INDEX.txt"
fi

save_case_evidence() {
    [ -n "$evidence_dir" ] || return 0
    local case_id="$1"
    local desc="$2"
    local cmd_str="$3"
    local rc="$4"
    local verdict="$5"
    local stdout_f="${6:-}"
    local stderr_f="${7:-}"
    local rcpt_f="${8:-}"
    local trc_f="${9:-}"

    echo "$cmd_str" > "$evidence_dir/cases/${case_id}.cmd"
    echo "$rc" > "$evidence_dir/cases/${case_id}.status"
    if [ -n "$stdout_f" ] && [ -f "$stdout_f" ]; then
        cp -f "$stdout_f" "$evidence_dir/cases/${case_id}.stdout"
    fi
    if [ -n "$stderr_f" ] && [ -f "$stderr_f" ]; then
        cp -f "$stderr_f" "$evidence_dir/cases/${case_id}.stderr"
    fi
    if [ -n "$rcpt_f" ] && [ -f "$rcpt_f" ]; then
        cp -f "$rcpt_f" "$evidence_dir/cases/${case_id}.receipt"
    fi
    if [ -n "$trc_f" ] && [ -f "$trc_f" ]; then
        cp -f "$trc_f" "$evidence_dir/cases/${case_id}.trace"
    fi
    printf "%-32s | %-9s | %-7s | %s\n" "$case_id" "$rc" "$verdict" "$desc" >> "$evidence_dir/INDEX.txt"
}

# Compile candidate sfo binary under pinned 2048 KiB stack with mandatory non-recovering ASan/UBSan
sfo_bin="$tmp/sfo"
if ! "$CC" -O2 -Wall -Wextra $SAN_FLAGS \
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

if [ -n "$evidence_dir" ]; then
    "$CC" --version > "$evidence_dir/compiler_info.txt"
    echo "BUILD_FLAGS: -O2 -Wall -Wextra $SAN_FLAGS" >> "$evidence_dir/compiler_info.txt"
    echo "WRAP_FLAGS: -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 $SAN_FLAGS -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=fseek -Wl,--wrap=ftell -Wl,--wrap=fread -Wl,--wrap=fwrite -Wl,--wrap=fputs -Wl,--wrap=fclose -Wl,--wrap=putchar -Wl,--wrap=putc -Wl,--wrap=fputc -Wl,--wrap=puts -Wl,--wrap=printf -Wl,--wrap=vprintf -Wl,--wrap=vfprintf -Wl,--wrap=fflush" >> "$evidence_dir/compiler_info.txt"

    sha256sum "$repo_root/tools/sfo-pkg/sfo.c" \
              "$golden_dir/input.xml" \
              "$repo_root/tests/sdk/pkg-test.sh" \
              "$expected_sfo" \
              "$expected_list" \
              "$expected_xml" \
              "$expected_roundtrip" > "$evidence_dir/input_manifest.txt"

    sha256sum "$repo_root/tools/sfo-pkg/sfo.c" \
              "$golden_dir/input.xml" \
              "$repo_root/tests/sdk/pkg-test.sh" \
              "$expected_sfo" \
              "$expected_list" \
              "$expected_xml" \
              "$expected_roundtrip" > "$evidence_dir/source_input_hashes_before.txt"
fi

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

    cand_toxml_log="$tmp/cand_toxml.log"

    rc_fx=0
    "$sfo_bin" --fromxml "$golden_dir/input.xml" "$cand_sfo" 2> "$cand_fromxml_err" || rc_fx=$?
    rc_l=0
    "$sfo_bin" -l "$cand_sfo" > "$cand_list_log" 2> "$cand_list_err" || rc_l=$?
    rc_tx=0
    "$sfo_bin" -t "$cand_sfo" "$cand_exported_xml" > "$cand_toxml_log" 2> "$cand_toxml_err" || rc_tx=$?
    rc_rt=0
    "$sfo_bin" --fromxml "$cand_exported_xml" "$cand_roundtrip" 2> "$cand_rt_err" || rc_rt=$?

    for pair in "fromxml:$rc_fx:$cand_fromxml_err" "list:$rc_l:$cand_list_err" "toxml:$rc_tx:$cand_toxml_err" "roundtrip:$rc_rt:$cand_rt_err"; do
        op="${pair%%:*}"
        rest="${pair#*:}"
        rc="${rest%%:*}"
        err="${rest#*:}"
        if [ "$rc" -ne 0 ]; then
            row_fail "Row (e)" "golden candidate operation '$op' exited with code $rc, expected 0"
            row_e_ok=0
        fi
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
    if ! cmp -s "$expected_list" "$cand_toxml_log"; then
        row_fail "Row (e)" "candidate sfo -t stdout dict mismatch against golden baseline list"
        row_e_ok=0
    fi
fi

# Judge helper for sfo invocations:
# Asserts exact exit status, verifies expected diagnostic, rejects sanitizer diagnostics,
# and enforces empty stderr when expected_diag is empty.
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
    if [ -z "$expected_diag" ] && [ -s "$err_file" ]; then
        row_fail "Row (e)" "$context: expected empty stderr, but found non-empty stderr ($(cat "$err_file"))"
        row_e_ok=0
        return 1
    fi
    if grep -E "Sanitizer|runtime error:" "$err_file" >/dev/null 2>&1; then
        row_fail "Row (e)" "$context: sanitizer error detected in $err_file"
        row_e_ok=0
        return 1
    fi
    return 0
}

# Unit controls on judge_sfo_invocation:
# Verify that matching diagnostic with a failing/mismatched exit code is rejected,
# missing diagnostic is rejected, and non-empty stderr with expected_diag="" is rejected.
ctrl_diag_err="$tmp/judge_ctrl.err"
printf 'SFO: out of memory\n' > "$ctrl_diag_err"
judge_ctrl_failed=0
if ( judge_sfo_invocation 0 1 "SFO: out of memory" "$ctrl_diag_err" "judge-control: exit 1 with expected diagnostic" ) >/dev/null 2>&1; then
    judge_ctrl_failed=$((judge_ctrl_failed + 1))
fi
if ( judge_sfo_invocation 1 1 "NONEXISTENT_DIAGNOSTIC_NEEDLE" "$ctrl_diag_err" "judge-control: missing diagnostic" ) >/dev/null 2>&1; then
    judge_ctrl_failed=$((judge_ctrl_failed + 1))
fi
ctrl_stray_err="$tmp/judge_ctrl_stray.err"
printf 'stray warning\n' > "$ctrl_stray_err"
if ( judge_sfo_invocation 0 0 "" "$ctrl_stray_err" "judge-control: stray stderr when empty expected" ) >/dev/null 2>&1; then
    judge_ctrl_failed=$((judge_ctrl_failed + 1))
fi
if [ "$judge_ctrl_failed" -ne 0 ]; then
    row_fail "Row (e)" "judge control failed: permitted wrong exit status, missing diagnostic, or stray stderr"
    row_e_ok=0
fi

# Judge helper for verifying deterministic injection receipts:
assert_injection_hit() {
    local expected_site="$1"
    local receipt_file="$2"
    local context="$3"

    if [ ! -f "$receipt_file" ] || ! grep -q "^\[INJECT_HIT: ${expected_site}\]$" "$receipt_file"; then
        row_fail "Row (e)" "$context: expected injection hit for site '${expected_site}' not found in receipt ($receipt_file)"
        row_e_ok=0
        return 1
    fi
    return 0
}

# Unit controls on assert_injection_hit:
# Verify that missing receipt, wrong site, or never-fired injection is rejected.
ctrl_receipt="$tmp/judge_ctrl_inject.receipt"
rm -f "$ctrl_receipt"
inject_ctrl_failed=0
if ( assert_injection_hit "read_buf" "$ctrl_receipt" "ctrl: missing receipt" ) >/dev/null 2>&1; then
    inject_ctrl_failed=$((inject_ctrl_failed + 1))
fi
printf '[INJECT_HIT: wrong_site]\n' > "$ctrl_receipt"
if ( assert_injection_hit "read_buf" "$ctrl_receipt" "ctrl: wrong site" ) >/dev/null 2>&1; then
    inject_ctrl_failed=$((inject_ctrl_failed + 1))
fi
if [ "$inject_ctrl_failed" -ne 0 ]; then
    row_fail "Row (e)" "injection control failed: permitted missing receipt or wrong site"
    row_e_ok=0
fi

# ---------------------------------------------------------------------------
# CLI syntax, options, and precedence checks
# ---------------------------------------------------------------------------
if [ "$row_e_ok" -eq 1 ]; then
    # Case 1: CLI-Help (-h exits 0, usage on stdout, clean stderr)
    sfo_help_out="$tmp/sfo_help.out"
    sfo_help_err="$tmp/sfo_help.err"
    rc_help=0
    "$sfo_bin" -h > "$sfo_help_out" 2> "$sfo_help_err" || rc_help=$?
    case_ok=1
    judge_sfo_invocation 0 "$rc_help" "" "$sfo_help_err" "CLI-Help (-h)" || case_ok=0
    if ! grep -F "usage:" "$sfo_help_out" >/dev/null 2>&1; then
        row_fail "Row (e)" "CLI-Help (-h) stdout missing 'usage:'"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_01_cli_help" "CLI-Help (-h)" "$sfo_bin -h" "$rc_help" "$v" "$sfo_help_out" "$sfo_help_err"

    # Case 2: CLI-Version (-v exits 0, version on stdout, clean stderr)
    sfo_ver_out="$tmp/sfo_ver.out"
    sfo_ver_err="$tmp/sfo_ver.err"
    rc_ver=0
    "$sfo_bin" -v > "$sfo_ver_out" 2> "$sfo_ver_err" || rc_ver=$?
    case_ok=1
    judge_sfo_invocation 0 "$rc_ver" "" "$sfo_ver_err" "CLI-Version (-v)" || case_ok=0
    if ! grep -F "sfo 0.3" "$sfo_ver_out" >/dev/null 2>&1; then
        row_fail "Row (e)" "CLI-Version (-v) stdout missing 'sfo 0.3'"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_02_cli_version" "CLI-Version (-v)" "$sfo_bin -v" "$rc_ver" "$v" "$sfo_ver_out" "$sfo_ver_err"

    # Case 3: CLI-Usage-Unknown (--unknown-flag exits 2, usage on stdout, getopt diagnostic on stderr)
    sfo_unk_out="$tmp/sfo_unk.out"
    sfo_unk_err="$tmp/sfo_unk.err"
    rc_unk=0
    "$sfo_bin" --unknown-flag > "$sfo_unk_out" 2> "$sfo_unk_err" || rc_unk=$?
    case_ok=1
    judge_sfo_invocation 2 "$rc_unk" "unrecognized option" "$sfo_unk_err" "CLI-Usage-Unknown (--unknown-flag)" || case_ok=0
    if ! grep -F "usage:" "$sfo_unk_out" >/dev/null 2>&1; then
        row_fail "Row (e)" "CLI-Usage-Unknown stdout missing 'usage:'"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_03_cli_usage_unknown" "CLI-Usage-Unknown (--unknown-flag)" "$sfo_bin --unknown-flag" "$rc_unk" "$v" "$sfo_unk_out" "$sfo_unk_err"

    # Case 4: CLI-Usage-Args (-t with missing required positional arguments exits 2)
    sfo_args_out="$tmp/sfo_args.out"
    sfo_args_err="$tmp/sfo_args.err"
    rc_args=0
    "$sfo_bin" -t "$cand_sfo" > "$sfo_args_out" 2> "$sfo_args_err" || rc_args=$?
    case_ok=1
    judge_sfo_invocation 2 "$rc_args" "" "$sfo_args_err" "CLI-Usage-Args (-t with 1 arg)" || case_ok=0
    if ! grep -F "usage:" "$sfo_args_out" >/dev/null 2>&1; then
        row_fail "Row (e)" "CLI-Usage-Args stdout missing 'usage:'"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_04_cli_usage_args" "CLI-Usage-Args (-t with 1 arg)" "$sfo_bin -t $cand_sfo" "$rc_args" "$v" "$sfo_args_out" "$sfo_args_err"

    # Case 5: CLI-Usage-Conflicting (-t and -f without -l exits 2)
    sfo_conf_out="$tmp/sfo_conf.out"
    sfo_conf_err="$tmp/sfo_conf.err"
    rc_conf=0
    "$sfo_bin" -t -f "$cand_sfo" "$tmp/out_conf.xml" > "$sfo_conf_out" 2> "$sfo_conf_err" || rc_conf=$?
    case_ok=1
    judge_sfo_invocation 2 "$rc_conf" "" "$sfo_conf_err" "CLI-Usage-Conflicting (-t and -f)" || case_ok=0
    if ! grep -F "usage:" "$sfo_conf_out" >/dev/null 2>&1; then
        row_fail "Row (e)" "CLI-Usage-Conflicting stdout missing 'usage:'"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_05_cli_usage_conflicting" "CLI-Usage-Conflicting (-t and -f)" "$sfo_bin -t -f $cand_sfo $tmp/out_conf.xml" "$rc_conf" "$v" "$sfo_conf_out" "$sfo_conf_err"

    # Case 6: CLI-Precedence (-l takes precedence over -t; destination file not created)
    sfo_prec_out="$tmp/sfo_prec.out"
    sfo_prec_err="$tmp/sfo_prec.err"
    sfo_prec_target="$tmp/prec_should_not_exist.xml"
    rm -f "$sfo_prec_target"
    rc_prec=0
    "$sfo_bin" -l "$cand_sfo" -t "$cand_sfo" "$sfo_prec_target" > "$sfo_prec_out" 2> "$sfo_prec_err" || rc_prec=$?
    case_ok=1
    judge_sfo_invocation 0 "$rc_prec" "" "$sfo_prec_err" "CLI-Precedence (-l precedence over -t)" || case_ok=0
    if ! cmp -s "$expected_list" "$sfo_prec_out"; then
        row_fail "Row (e)" "CLI-Precedence output mismatch against golden list"
        row_e_ok=0
        case_ok=0
    fi
    if [ -e "$sfo_prec_target" ]; then
        row_fail "Row (e)" "CLI-Precedence created -t output file ($sfo_prec_target) despite -l taking precedence"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_06_cli_precedence" "CLI-Precedence (-l precedence over -t)" "$sfo_bin -l $cand_sfo -t $cand_sfo $sfo_prec_target" "$rc_prec" "$v" "$sfo_prec_out" "$sfo_prec_err"
fi

# ---------------------------------------------------------------------------
# File reading, validation errors, and destination lifecycle (no-unlink policy)
# ---------------------------------------------------------------------------
if [ "$row_e_ok" -eq 1 ]; then
    # Case 7: Err-Read-Missing (nonexistent input file exits 1 with perror diagnostic)
    sfo_nonexistent_err="$tmp/sfo_nonexistent.err"
    rc_nonexistent=0
    "$sfo_bin" -l "$tmp/nonexistent.sfo" >/dev/null 2> "$sfo_nonexistent_err" || rc_nonexistent=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_nonexistent" "nonexistent.sfo" "$sfo_nonexistent_err" "Err-Read-Missing (nonexistent input file)" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_07_err_read_missing" "Err-Read-Missing (nonexistent input file)" "$sfo_bin -l $tmp/nonexistent.sfo" "$rc_nonexistent" "$v" "" "$sfo_nonexistent_err"

    # Case 8: Err-Read-Small (file < 20 bytes exits 1 with 'SFO: file too small')
    printf 'SHORT_FILE' > "$tmp/too_small.sfo"
    sfo_small_err="$tmp/sfo_too_small.err"
    rc_small=0
    "$sfo_bin" -l "$tmp/too_small.sfo" >/dev/null 2> "$sfo_small_err" || rc_small=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_small" "SFO: file too small" "$sfo_small_err" "Err-Read-Small (too-small input file)" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_08_err_read_small" "Err-Read-Small (too-small input file)" "$sfo_bin -l $tmp/too_small.sfo" "$rc_small" "$v" "" "$sfo_small_err"

    # Case 9: Err-Read-Magic (-l with corrupt header exits 1 with 'SFO: invalid header')
    head -c 24 /dev/zero > "$tmp/corrupt.sfo"
    sfo_corrupt_l_err="$tmp/sfo_corrupt_l.err"
    rc_corrupt_l=0
    "$sfo_bin" -l "$tmp/corrupt.sfo" >/dev/null 2> "$sfo_corrupt_l_err" || rc_corrupt_l=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_corrupt_l" "SFO: invalid header" "$sfo_corrupt_l_err" "Err-Read-Magic (list_sfo corrupt header)" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_09_err_read_magic_list" "Err-Read-Magic (list_sfo corrupt header)" "$sfo_bin -l $tmp/corrupt.sfo" "$rc_corrupt_l" "$v" "" "$sfo_corrupt_l_err"

    # Case 10: Err-Read-Magic (-t with corrupt header exits 1 with 'SFO: bad magic 00000000', no output file created)
    sfo_corrupt_t_err="$tmp/sfo_corrupt_t.err"
    sfo_corrupt_t_out="$tmp/out_corrupt.xml"
    rm -f "$sfo_corrupt_t_out"
    rc_corrupt_t=0
    "$sfo_bin" -t "$tmp/corrupt.sfo" "$sfo_corrupt_t_out" >/dev/null 2> "$sfo_corrupt_t_err" || rc_corrupt_t=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_corrupt_t" "SFO: bad magic 00000000" "$sfo_corrupt_t_err" "Err-Read-Magic (convert_to_xml bad magic)" || case_ok=0
    if [ -e "$sfo_corrupt_t_out" ]; then
        row_fail "Row (e)" "Err-Read-Magic created output XML ($sfo_corrupt_t_out)"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_10_err_read_magic_toxml" "Err-Read-Magic (convert_to_xml bad magic)" "$sfo_bin -t $tmp/corrupt.sfo $sfo_corrupt_t_out" "$rc_corrupt_t" "$v" "" "$sfo_corrupt_t_err"

    # Case 11: Err-Open-Absent (output file open failure with absent destination path remains absent)
    sfo_outfail_err="$tmp/sfo_outfail.err"
    outfail_target="$tmp/nonexistent_dir_absent/output.xml"
    rc_outfail=0
    "$sfo_bin" -t "$cand_sfo" "$outfail_target" >/dev/null 2> "$sfo_outfail_err" || rc_outfail=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_outfail" "nonexistent_dir_absent" "$sfo_outfail_err" "Err-Open-Absent (output open failure with absent destination)" || case_ok=0
    if [ -e "$outfail_target" ]; then
        row_fail "Row (e)" "Err-Open-Absent created output file ($outfail_target)"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_11_err_open_absent" "Err-Open-Absent (output open failure with absent destination)" "$sfo_bin -t $cand_sfo $outfail_target" "$rc_outfail" "$v" "" "$sfo_outfail_err"

    # Case 12: Err-Open-Sentinel (pre-open failure preserves pre-existing destination byte-identically without unlinking)
    sentinel_target="$tmp/sentinel_pre_existing.xml"
    printf 'SENTINEL_EXACT_PRESERVED_BYTES_1234567890\n' > "$sentinel_target"
    sentinel_sha_before=$(sha256sum "$sentinel_target" 2>/dev/null | awk '{print $1}' || shasum -a 256 "$sentinel_target" | awk '{print $1}')
    sfo_sentinel_err="$tmp/sfo_sentinel.err"
    rc_sentinel=0
    "$sfo_bin" -t "$tmp/corrupt.sfo" "$sentinel_target" >/dev/null 2> "$sfo_sentinel_err" || rc_sentinel=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_sentinel" "SFO: bad magic" "$sfo_sentinel_err" "Err-Open-Sentinel (pre-existing destination preservation)" || case_ok=0
    sentinel_sha_after=$(sha256sum "$sentinel_target" 2>/dev/null | awk '{print $1}' || shasum -a 256 "$sentinel_target" | awk '{print $1}')
    if [ "$sentinel_sha_before" != "$sentinel_sha_after" ]; then
        row_fail "Row (e)" "Err-Open-Sentinel corrupted or modified pre-existing destination"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_12_err_open_sentinel" "Err-Open-Sentinel (pre-existing destination preservation)" "$sfo_bin -t $tmp/corrupt.sfo $sentinel_target" "$rc_sentinel" "$v" "" "$sfo_sentinel_err"

    # Full disk / stream write failures (/dev/full)
    if [ -c /dev/full ]; then
        # Case 13: convert_to_xml write to /dev/full (exit 1, diagnostic with /dev/full)
        sfo_full_xml_err="$tmp/sfo_full_xml.err"
        rc_full_xml=0
        "$sfo_bin" -t "$cand_sfo" /dev/full >/dev/null 2> "$sfo_full_xml_err" || rc_full_xml=$?
        case_ok=1
        judge_sfo_invocation 1 "$rc_full_xml" "/dev/full" "$sfo_full_xml_err" "Err-Write-Full-XML (/dev/full write failure)" || case_ok=0
        [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
        save_case_evidence "case_13_err_write_full_xml" "Err-Write-Full-XML (/dev/full write failure)" "$sfo_bin -t $cand_sfo /dev/full" "$rc_full_xml" "$v" "" "$sfo_full_xml_err"

        # Case 14: convert_from_xml write to /dev/full (exit 1, diagnostic with /dev/full)
        sfo_full_sfo_err="$tmp/sfo_full_sfo.err"
        rc_full_sfo=0
        "$sfo_bin" -f "$cand_exported_xml" /dev/full >/dev/null 2> "$sfo_full_sfo_err" || rc_full_sfo=$?
        case_ok=1
        judge_sfo_invocation 1 "$rc_full_sfo" "/dev/full" "$sfo_full_sfo_err" "Err-Write-Full-SFO (/dev/full write failure)" || case_ok=0
        [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
        save_case_evidence "case_14_err_write_full_sfo" "Err-Write-Full-SFO (/dev/full write failure)" "$sfo_bin -f $cand_exported_xml /dev/full" "$rc_full_sfo" "$v" "" "$sfo_full_sfo_err"

        # Case 15: stdout write/flush failure in list_sfo (exit 1, 'stdout: No space left on device')
        sfo_full_l_err="$tmp/sfo_full_l.err"
        rc_full_l=0
        "$sfo_bin" -l "$cand_sfo" > /dev/full 2> "$sfo_full_l_err" || rc_full_l=$?
        case_ok=1
        judge_sfo_invocation 1 "$rc_full_l" "stdout" "$sfo_full_l_err" "Err-Stdout-Full-List (stdout flush failure in -l)" || case_ok=0
        [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
        save_case_evidence "case_15_err_stdout_full_list" "Err-Stdout-Full-List (stdout flush failure in -l)" "$sfo_bin -l $cand_sfo > /dev/full" "$rc_full_l" "$v" "" "$sfo_full_l_err"

        # Case 16: stdout write/flush failure in convert_to_xml (exit 1, 'stdout: No space left on device', output file not created)
        sfo_full_t_err="$tmp/sfo_full_t.err"
        sfo_full_t_out="$tmp/stdout_full_no_out.xml"
        rm -f "$sfo_full_t_out"
        rc_full_t=0
        "$sfo_bin" -t "$cand_sfo" "$sfo_full_t_out" > /dev/full 2> "$sfo_full_t_err" || rc_full_t=$?
        case_ok=1
        judge_sfo_invocation 1 "$rc_full_t" "stdout" "$sfo_full_t_err" "Err-Stdout-Full-ToXML (stdout flush failure in -t)" || case_ok=0
        if [ -e "$sfo_full_t_out" ]; then
            row_fail "Row (e)" "Err-Stdout-Full-ToXML created output XML ($sfo_full_t_out) despite stdout failure"
            row_e_ok=0
            case_ok=0
        fi
        [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
        save_case_evidence "case_16_err_stdout_full_toxml" "Err-Stdout-Full-ToXML (stdout flush failure in -t)" "$sfo_bin -t $cand_sfo $sfo_full_t_out > /dev/full" "$rc_full_t" "$v" "" "$sfo_full_t_err"
    else
        note "Row (e): /dev/full not available on this host, skipping full-disk tests"
    fi
fi

# ---------------------------------------------------------------------------
# Bounded XML parsing error contract
# ---------------------------------------------------------------------------
if [ "$row_e_ok" -eq 1 ]; then
    # Case 17: Err-XML-Empty (zero valid entries exits 1 with 'SFO: no valid entries in XML')
    empty_xml="$tmp/empty.xml"
    printf '<?xml version="1.0" ?>\n<sfo>\n</sfo>\n' > "$empty_xml"
    out_empty="$tmp/empty_out.sfo"
    rm -f "$out_empty"
    sfo_empty_err="$tmp/sfo_empty.err"
    rc_empty=0
    "$sfo_bin" -f "$empty_xml" "$out_empty" >/dev/null 2> "$sfo_empty_err" || rc_empty=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_empty" "SFO: no valid entries in XML" "$sfo_empty_err" "Err-XML-Empty (zero valid entries)" || case_ok=0
    if [ -e "$out_empty" ]; then
        row_fail "Row (e)" "Err-XML-Empty created output SFO ($out_empty)"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_17_err_xml_empty" "Err-XML-Empty (zero valid entries)" "$sfo_bin -f $empty_xml $out_empty" "$rc_empty" "$v" "" "$sfo_empty_err"

    # Case 18: Err-XML-Unclosed (unclosed tag exits 1 with 'SFO: malformed XML tag')
    unclosed_xml="$tmp/unclosed.xml"
    printf '<?xml version="1.0" ?>\n<sfo>\n\t<value name="TEST" type="string" unclosed tag\n</sfo>\n' > "$unclosed_xml"
    out_unclosed="$tmp/unclosed_out.sfo"
    rm -f "$out_unclosed"
    sfo_unclosed_err="$tmp/sfo_unclosed.err"
    rc_unclosed=0
    "$sfo_bin" -f "$unclosed_xml" "$out_unclosed" >/dev/null 2> "$sfo_unclosed_err" || rc_unclosed=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_unclosed" "SFO: malformed XML tag" "$sfo_unclosed_err" "Err-XML-Unclosed (unclosed tag)" || case_ok=0
    if [ -e "$out_unclosed" ]; then
        row_fail "Row (e)" "Err-XML-Unclosed created output SFO ($out_unclosed)"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_18_err_xml_unclosed" "Err-XML-Unclosed (unclosed tag)" "$sfo_bin -f $unclosed_xml $out_unclosed" "$rc_unclosed" "$v" "" "$sfo_unclosed_err"

    # Case 19: Err-XML-AfterVal (malformed tag after valid entry aborts immediately without emitting SFO)
    afterval_xml="$tmp/afterval.xml"
    cat > "$afterval_xml" << 'EOF'
<?xml version="1.0" ?>
<sfo>
	<value name="APP_VER" type="string">01.00</value>
	<value name="BROKEN" type="string">missing closing tag
</sfo>
EOF
    out_afterval="$tmp/afterval_out.sfo"
    rm -f "$out_afterval"
    sfo_afterval_err="$tmp/sfo_afterval.err"
    rc_afterval=0
    "$sfo_bin" -f "$afterval_xml" "$out_afterval" >/dev/null 2> "$sfo_afterval_err" || rc_afterval=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_afterval" "SFO: malformed XML tag" "$sfo_afterval_err" "Err-XML-AfterVal (malformed tag after valid entry)" || case_ok=0
    if [ -e "$out_afterval" ]; then
        row_fail "Row (e)" "Err-XML-AfterVal created output SFO ($out_afterval)"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_19_err_xml_afterval" "Err-XML-AfterVal (malformed tag after valid entry)" "$sfo_bin -f $afterval_xml $out_afterval" "$rc_afterval" "$v" "" "$sfo_afterval_err"

    # Case 19b: Err-XML-BareAtEOF (truncated bare '<value' at EOF after valid entry exits 1 with 'SFO: malformed XML tag')
    bare_xml="$tmp/bare_at_eof.xml"
    printf '<value name="APP_VER" type="string">01.00</value><value' > "$bare_xml"
    # Subcase (i): Absent destination lifecycle
    out_bare_absent="$tmp/bare_absent.sfo"
    rm -f "$out_bare_absent"
    sfo_bare_err="$tmp/sfo_bare.err"
    rc_bare=0
    "$sfo_bin" -f "$bare_xml" "$out_bare_absent" >/dev/null 2> "$sfo_bare_err" || rc_bare=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_bare" "SFO: malformed XML tag" "$sfo_bare_err" "Err-XML-BareAtEOF-Absent (truncated <value at EOF, absent destination)" || case_ok=0
    if [ -e "$out_bare_absent" ]; then
        row_fail "Row (e)" "Err-XML-BareAtEOF created output SFO ($out_bare_absent) on absent destination"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_19b_bare_at_eof_absent" "Err-XML-BareAtEOF-Absent" "$sfo_bin -f $bare_xml $out_bare_absent" "$rc_bare" "$v" "" "$sfo_bare_err"

    # Subcase (ii): Existing sentinel destination lifecycle
    out_bare_sentinel="$tmp/bare_sentinel.sfo"
    sentinel_ref="$tmp/sentinel_ref.dat"
    printf 'SENTINEL_DO_NOT_OVERWRITE_0123456789' > "$sentinel_ref"
    cp -f "$sentinel_ref" "$out_bare_sentinel"
    sfo_bare_sent_err="$tmp/sfo_bare_sent.err"
    rc_bare_sent=0
    "$sfo_bin" -f "$bare_xml" "$out_bare_sentinel" >/dev/null 2> "$sfo_bare_sent_err" || rc_bare_sent=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_bare_sent" "SFO: malformed XML tag" "$sfo_bare_sent_err" "Err-XML-BareAtEOF-Sentinel (truncated <value at EOF, sentinel destination)" || case_ok=0
    if ! cmp -s "$sentinel_ref" "$out_bare_sentinel"; then
        row_fail "Row (e)" "Err-XML-BareAtEOF mutated pre-existing sentinel output ($out_bare_sentinel)"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_19b_bare_at_eof_sentinel" "Err-XML-BareAtEOF-Sentinel" "$sfo_bin -f $bare_xml $out_bare_sentinel" "$rc_bare_sent" "$v" "" "$sfo_bare_sent_err"
fi

# ---------------------------------------------------------------------------
# Deterministic allocation and I/O failures via linker wrap
# ---------------------------------------------------------------------------
cat > "$tmp/wrap_sfo.c" << 'EOF'
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t nmemb, size_t size);
int   __real_fseek(FILE *fp, long offset, int whence);
long  __real_ftell(FILE *fp);
size_t __real_fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t __real_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int   __real_fputs(const char *s, FILE *fp);
int   __real_fputc(int c, FILE *fp);
int   __real_putc(int c, FILE *fp);
int   __real_fclose(FILE *fp);
int   __real_putchar(int c);
int   __real_puts(const char *s);
int   __real_printf(const char *fmt, ...);
int   __real_vprintf(const char *fmt, va_list ap);
int   __real_vfprintf(FILE *fp, const char *fmt, va_list ap);
int   __real_fflush(FILE *fp);

static int s_in_wrap = 0;

static void record_hit(const char *site) {
    int saved = errno;
    const char *receipt = getenv("INJECT_RECEIPT");
    if (receipt && *receipt) {
        int fd = open(receipt, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            char buf[256];
            int n = snprintf(buf, sizeof(buf), "[INJECT_HIT: %s]\n", site);
            if (n > 0) {
                (void)write(fd, buf, (size_t)n);
            }
            close(fd);
        }
    }
    errno = saved;
}

static void record_trace(const char *kind, size_t count, size_t size, int ordinal, const char *status) {
    int saved = errno;
    const char *trace = getenv("INJECT_TRACE");
    if (trace && *trace) {
        int fd = open(trace, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            char buf[256];
            int n = snprintf(buf, sizeof(buf), "ALLOC %d: %s(count=%zu, size=%zu) -> %s\n",
                             ordinal, kind, count, size, status);
            if (n > 0) {
                (void)write(fd, buf, (size_t)n);
            }
            close(fd);
        }
    }
    errno = saved;
}

void *__wrap_calloc(size_t nmemb, size_t size) {
    if (s_in_wrap) return __real_calloc(nmemb, size);
    s_in_wrap = 1;
    static int s_calloc_count = 0;
    static int s_offset_idx = 0;
    s_calloc_count++;
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (inj) {
        if (strcmp(inj, "kvs") == 0 && nmemb == 256) {
            s_in_wrap = 0;
            record_trace("calloc", nmemb, size, s_calloc_count, "INJECT_FAIL_kvs");
            record_hit("kvs");
            return NULL;
        }
        if (size == sizeof(uint32_t)) {
            s_offset_idx++;
            if (strcmp(inj, "offset_key") == 0 && s_offset_idx == 1) {
                s_in_wrap = 0;
                record_trace("calloc", nmemb, size, s_calloc_count, "INJECT_FAIL_offset_key");
                record_hit("offset_key");
                return NULL;
            }
            if (strcmp(inj, "offset_val") == 0 && s_offset_idx == 2) {
                s_in_wrap = 0;
                record_trace("calloc", nmemb, size, s_calloc_count, "INJECT_FAIL_offset_val");
                record_hit("offset_val");
                return NULL;
            }
            if (strcmp(inj, "offset_pad") == 0 && s_offset_idx == 3) {
                s_in_wrap = 0;
                record_trace("calloc", nmemb, size, s_calloc_count, "INJECT_FAIL_offset_pad");
                record_hit("offset_pad");
                return NULL;
            }
            if (strcmp(inj, "offset_len") == 0 && s_offset_idx == 4) {
                s_in_wrap = 0;
                record_trace("calloc", nmemb, size, s_calloc_count, "INJECT_FAIL_offset_len");
                record_hit("offset_len");
                return NULL;
            }
        }
    }
    record_trace("calloc", nmemb, size, s_calloc_count, "OK");
    s_in_wrap = 0;
    return __real_calloc(nmemb, size);
}

void *__wrap_malloc(size_t size) {
    if (s_in_wrap) return __real_malloc(size);
    s_in_wrap = 1;
    static int s_malloc_count = 0;
    s_malloc_count++;
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (inj) {
        if (strcmp(inj, "read_buf") == 0 && s_malloc_count == 1) {
            s_in_wrap = 0;
            record_trace("malloc", 1, size, s_malloc_count, "INJECT_FAIL_read_buf");
            record_hit("read_buf");
            return NULL;
        }
        if (strcmp(inj, "tag_buf") == 0 && s_malloc_count == 2) {
            s_in_wrap = 0;
            record_trace("malloc", 1, size, s_malloc_count, "INJECT_FAIL_tag_buf");
            record_hit("tag_buf");
            return NULL;
        }
        if (strcmp(inj, "content") == 0 && s_malloc_count == 3) {
            s_in_wrap = 0;
            record_trace("malloc", 1, size, s_malloc_count, "INJECT_FAIL_content");
            record_hit("content");
            return NULL;
        }
        if (strcmp(inj, "override_title") == 0 && s_malloc_count == 22 && size == 81) {
            s_in_wrap = 0;
            record_trace("malloc", 1, size, s_malloc_count, "INJECT_FAIL_override_title");
            record_hit("override_title");
            return NULL;
        }
        if (strcmp(inj, "override_appid") == 0 && s_malloc_count == 24 && size == 77) {
            s_in_wrap = 0;
            record_trace("malloc", 1, size, s_malloc_count, "INJECT_FAIL_override_appid");
            record_hit("override_appid");
            return NULL;
        }
    }
    record_trace("malloc", 1, size, s_malloc_count, "OK");
    s_in_wrap = 0;
    return __real_malloc(size);
}

int __wrap_fseek(FILE *fp, long offset, int whence) {
    if (s_in_wrap) return __real_fseek(fp, offset, whence);
    s_in_wrap = 1;
    static int s_fseek_calls = 0;
    s_fseek_calls++;
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (inj) {
        if (strcmp(inj, "seek_first") == 0 && s_fseek_calls == 1 && whence == SEEK_END) {
            s_in_wrap = 0;
            record_hit("seek_first");
            errno = ESPIPE;
            return -1;
        }
        if (strcmp(inj, "seek_second") == 0 && s_fseek_calls == 2 && whence == SEEK_SET) {
            s_in_wrap = 0;
            record_hit("seek_second");
            errno = EINVAL;
            return -1;
        }
    }
    s_in_wrap = 0;
    return __real_fseek(fp, offset, whence);
}

long __wrap_ftell(FILE *fp) {
    if (s_in_wrap) return __real_ftell(fp);
    s_in_wrap = 1;
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (inj && strcmp(inj, "tell") == 0) {
        s_in_wrap = 0;
        record_hit("tell");
        errno = EOVERFLOW;
        return -1;
    }
    s_in_wrap = 0;
    return __real_ftell(fp);
}

size_t __wrap_fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (s_in_wrap) return __real_fread(ptr, size, nmemb, fp);
    s_in_wrap = 1;
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (inj && strcmp(inj, "read_short") == 0) {
        s_in_wrap = 0;
        record_hit("read_short");
        errno = EIO;
        return 0;
    }
    s_in_wrap = 0;
    return __real_fread(ptr, size, nmemb, fp);
}

size_t __wrap_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (s_in_wrap) return __real_fwrite(ptr, size, nmemb, fp);
    s_in_wrap = 1;
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (inj) {
        if (fp != stdout && fp != stderr) {
            if (strcmp(inj, "output_write") == 0) {
                s_in_wrap = 0;
                record_hit("output_write");
                errno = ENOSPC;
                return 0;
            }
            if (strcmp(inj, "write_then_close") == 0) {
                static int s_wtc_done = 0;
                if (!s_wtc_done) {
                    s_wtc_done = 1;
                    s_in_wrap = 0;
                    record_hit("output_write");
                    errno = ENOSPC;
                    return 0;
                }
            }
        }
    }
    s_in_wrap = 0;
    return __real_fwrite(ptr, size, nmemb, fp);
}

int __wrap_fputs(const char *s, FILE *fp) {
    if (s_in_wrap) return __real_fputs(s, fp);
    s_in_wrap = 1;
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (inj) {
        if (fp != stdout && fp != stderr) {
            if (strcmp(inj, "output_write") == 0) {
                s_in_wrap = 0;
                record_hit("output_write");
                errno = ENOSPC;
                return EOF;
            }
            if (strcmp(inj, "write_then_close") == 0) {
                static int s_wtc_done = 0;
                if (!s_wtc_done) {
                    s_wtc_done = 1;
                    s_in_wrap = 0;
                    record_hit("output_write");
                    errno = ENOSPC;
                    return EOF;
                }
            }
        } else if (fp == stdout) {
            if (strcmp(inj, "stdout_no_errno") == 0) {
                s_in_wrap = 0;
                record_hit("stdout_no_errno");
                errno = 0;
                return EOF;
            }
            if (strcmp(inj, "stdout_stale_before") == 0) {
                s_in_wrap = 0;
                record_hit("stdout_fail");
                return EOF;
            }
            if (strcmp(inj, "stdout_write_then_flush") == 0) {
                static int s_sw_done = 0;
                if (!s_sw_done) {
                    s_sw_done = 1;
                    s_in_wrap = 0;
                    record_hit("stdout_write");
                    errno = ENOSPC;
                    return EOF;
                }
            }
        }
    }
    s_in_wrap = 0;
    return __real_fputs(s, fp);
}

int __wrap_fclose(FILE *fp) {
    if (s_in_wrap) return __real_fclose(fp);
    s_in_wrap = 1;
    static int s_fclose_calls = 0;
    s_fclose_calls++;
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (inj) {
        if (strcmp(inj, "input_close") == 0 && s_fclose_calls == 1) {
            s_in_wrap = 0;
            record_hit("input_close");
            __real_fclose(fp);
            errno = EIO;
            return EOF;
        }
        if (strcmp(inj, "stdout_stale_before") == 0 && s_fclose_calls == 1) {
            s_in_wrap = 0;
            record_hit("stale_fclose");
            __real_fclose(fp);
            errno = E2BIG;
            return 0;
        }
        if (strcmp(inj, "output_close") == 0 && s_fclose_calls == 2 && fp != stdout && fp != stderr) {
            s_in_wrap = 0;
            record_hit("output_close");
            __real_fclose(fp);
            errno = EDQUOT;
            return EOF;
        }
        if (strcmp(inj, "write_then_close") == 0 && s_fclose_calls == 2 && fp != stdout && fp != stderr) {
            s_in_wrap = 0;
            record_hit("output_close");
            __real_fclose(fp);
            errno = EIO;
            return EOF;
        }
    }
    s_in_wrap = 0;
    return __real_fclose(fp);
}

static int handle_stdout_inj(const char *site_ret_eof) {
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (!inj) return 0;
    if (strcmp(inj, "stdout_no_errno") == 0) {
        s_in_wrap = 0;
        record_hit("stdout_no_errno");
        errno = 0;
        return 1;
    }
    if (strcmp(inj, "stdout_stale_before") == 0) {
        s_in_wrap = 0;
        record_hit("stdout_fail");
        return 1;
    }
    if (strcmp(inj, "stdout_write_then_flush") == 0) {
        static int s_sw_done = 0;
        if (!s_sw_done) {
            s_sw_done = 1;
            s_in_wrap = 0;
            record_hit("stdout_write");
            errno = ENOSPC;
            return 1;
        }
    }
    return 0;
}

int __wrap_putchar(int c) {
    if (s_in_wrap) return __real_putchar(c);
    s_in_wrap = 1;
    if (handle_stdout_inj("putchar")) return EOF;
    s_in_wrap = 0;
    return __real_putchar(c);
}

int __wrap_putc(int c, FILE *fp) {
    if (s_in_wrap) return __real_putc(c, fp);
    s_in_wrap = 1;
    if (fp == stdout) {
        if (handle_stdout_inj("putc")) return EOF;
    } else if (fp != stderr) {
        const char *inj = getenv("INJECT_SFO_FAIL");
        if (inj) {
            if (strcmp(inj, "output_write") == 0) {
                s_in_wrap = 0;
                record_hit("output_write");
                errno = ENOSPC;
                return EOF;
            }
            if (strcmp(inj, "write_then_close") == 0) {
                static int s_wtc_done = 0;
                if (!s_wtc_done) {
                    s_wtc_done = 1;
                    s_in_wrap = 0;
                    record_hit("output_write");
                    errno = ENOSPC;
                    return EOF;
                }
            }
        }
    }
    s_in_wrap = 0;
    return __real_putc(c, fp);
}

int __wrap_fputc(int c, FILE *fp) {
    if (s_in_wrap) return __real_fputc(c, fp);
    s_in_wrap = 1;
    if (fp == stdout) {
        if (handle_stdout_inj("fputc")) return EOF;
    } else if (fp != stderr) {
        const char *inj = getenv("INJECT_SFO_FAIL");
        if (inj) {
            if (strcmp(inj, "output_write") == 0) {
                s_in_wrap = 0;
                record_hit("output_write");
                errno = ENOSPC;
                return EOF;
            }
            if (strcmp(inj, "write_then_close") == 0) {
                static int s_wtc_done = 0;
                if (!s_wtc_done) {
                    s_wtc_done = 1;
                    s_in_wrap = 0;
                    record_hit("output_write");
                    errno = ENOSPC;
                    return EOF;
                }
            }
        }
    }
    s_in_wrap = 0;
    return __real_fputc(c, fp);
}

int __wrap_puts(const char *s) {
    if (s_in_wrap) return __real_puts(s);
    s_in_wrap = 1;
    if (handle_stdout_inj("puts")) return EOF;
    s_in_wrap = 0;
    return __real_puts(s);
}

int __wrap_vfprintf(FILE *fp, const char *fmt, va_list ap) {
    if (s_in_wrap) return __real_vfprintf(fp, fmt, ap);
    s_in_wrap = 1;
    if (fp == stdout) {
        if (handle_stdout_inj("vfprintf")) return -1;
    } else if (fp != stderr) {
        const char *inj = getenv("INJECT_SFO_FAIL");
        if (inj) {
            if (strcmp(inj, "output_write") == 0) {
                s_in_wrap = 0;
                record_hit("output_write");
                errno = ENOSPC;
                return -1;
            }
            if (strcmp(inj, "write_then_close") == 0) {
                static int s_wtc_done = 0;
                if (!s_wtc_done) {
                    s_wtc_done = 1;
                    s_in_wrap = 0;
                    record_hit("output_write");
                    errno = ENOSPC;
                    return -1;
                }
            }
        }
    }
    s_in_wrap = 0;
    return __real_vfprintf(fp, fmt, ap);
}

int __wrap_vprintf(const char *fmt, va_list ap) {
    return __wrap_vfprintf(stdout, fmt, ap);
}

int __wrap_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = __wrap_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return ret;
}

int __wrap_fflush(FILE *fp) {
    if (s_in_wrap) return __real_fflush(fp);
    s_in_wrap = 1;
    const char *inj = getenv("INJECT_SFO_FAIL");
    if (inj && fp == stdout) {
        if (strcmp(inj, "stdout_write_then_flush") == 0) {
            s_in_wrap = 0;
            record_hit("stdout_flush");
            errno = EIO;
            return EOF;
        }
    }
    s_in_wrap = 0;
    return __real_fflush(fp);
}
EOF

sfo_wrap_bin="$tmp/sfo_wrap"
if ! "$CC" -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 $SAN_FLAGS \
    "$repo_root/tools/sfo-pkg/sfo.c" \
    "$tmp/wrap_sfo.c" \
    -Wl,--wrap=malloc -Wl,--wrap=calloc \
    -Wl,--wrap=fseek -Wl,--wrap=ftell \
    -Wl,--wrap=fread -Wl,--wrap=fwrite \
    -Wl,--wrap=fputs -Wl,--wrap=fclose \
    -Wl,--wrap=putchar -Wl,--wrap=putc -Wl,--wrap=fputc -Wl,--wrap=puts -Wl,--wrap=printf -Wl,--wrap=vprintf -Wl,--wrap=vfprintf -Wl,--wrap=fflush \
    -o "$sfo_wrap_bin" > "$tmp/sfo_wrap_build.log" 2>&1; then
    row_fail "Row (e)" "sfo_wrap compilation with wrapped allocators and I/O failed (see $tmp/sfo_wrap_build.log)"
    row_e_ok=0
fi

if [ "$row_e_ok" -eq 1 ]; then
    # Case 20a: list_sfo kvs calloc OOM (exit 1, 'SFO: out of memory')
    oom_list_err="$tmp/oom_list.err"
    rcpt_20a="$tmp/rcpt_20a.log"; rm -f "$rcpt_20a"
    rc_oom_list=0
    cmd_20a="INJECT_RECEIPT="$rcpt_20a" INJECT_SFO_FAIL=kvs $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20a" INJECT_SFO_FAIL=kvs "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$oom_list_err" || rc_oom_list=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_list" "SFO: out of memory" "$oom_list_err" "Case 20a: list_sfo kvs calloc OOM" || case_ok=0
    assert_injection_hit "kvs" "$rcpt_20a" "Case 20a: list_sfo kvs calloc hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20a_oom_kvs_list" "list_sfo kvs calloc OOM" "$cmd_20a" "$rc_oom_list" "$v" "" "$oom_list_err" "$rcpt_20a"

    # Case 20b: convert_to_xml kvs calloc OOM (exit 1, 'SFO: out of memory', no output file created)
    oom_toxml_err="$tmp/oom_toxml.err"
    oom_toxml_out="$tmp/oom.xml"
    rm -f "$oom_toxml_out"
    rcpt_20b="$tmp/rcpt_20b.log"; rm -f "$rcpt_20b"
    rc_oom_toxml=0
    cmd_20b="INJECT_RECEIPT="$rcpt_20b" INJECT_SFO_FAIL=kvs $sfo_wrap_bin -t $cand_sfo $oom_toxml_out"
    INJECT_RECEIPT="$rcpt_20b" INJECT_SFO_FAIL=kvs "$sfo_wrap_bin" -t "$cand_sfo" "$oom_toxml_out" >/dev/null 2> "$oom_toxml_err" || rc_oom_toxml=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_toxml" "SFO: out of memory" "$oom_toxml_err" "Case 20b: convert_to_xml kvs calloc OOM" || case_ok=0
    assert_injection_hit "kvs" "$rcpt_20b" "Case 20b: convert_to_xml kvs calloc hit receipt" || case_ok=0
    if [ -e "$oom_toxml_out" ]; then
        row_fail "Row (e)" "convert_to_xml created output file on kvs OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20b_oom_kvs_toxml" "convert_to_xml kvs calloc OOM" "$cmd_20b" "$rc_oom_toxml" "$v" "" "$oom_toxml_err" "$rcpt_20b"

    # Case 20c: convert_from_xml kvs calloc OOM (exit 1, 'SFO: out of memory', no output file created)
    oom_fromxml_err="$tmp/oom_fromxml.err"
    oom_fromxml_out="$tmp/oom_kvs.sfo"
    rm -f "$oom_fromxml_out"
    rcpt_20c="$tmp/rcpt_20c.log"; rm -f "$rcpt_20c"
    rc_oom_fromxml=0
    cmd_20c="INJECT_RECEIPT="$rcpt_20c" INJECT_SFO_FAIL=kvs $sfo_wrap_bin --fromxml $cand_exported_xml $oom_fromxml_out"
    INJECT_RECEIPT="$rcpt_20c" INJECT_SFO_FAIL=kvs "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$oom_fromxml_out" >/dev/null 2> "$oom_fromxml_err" || rc_oom_fromxml=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_fromxml" "SFO: out of memory" "$oom_fromxml_err" "Case 20c: convert_from_xml kvs calloc OOM" || case_ok=0
    assert_injection_hit "kvs" "$rcpt_20c" "Case 20c: convert_from_xml kvs calloc hit receipt" || case_ok=0
    if [ -e "$oom_fromxml_out" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on kvs OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20c_oom_kvs_fromxml" "convert_from_xml kvs calloc OOM" "$cmd_20c" "$rc_oom_fromxml" "$v" "" "$oom_fromxml_err" "$rcpt_20c"

    # Case 20d: read_file malloc failure (exit 1, 'SFO: out of memory')
    oom_read_err="$tmp/oom_read.err"
    rcpt_20d="$tmp/rcpt_20d.log"; rm -f "$rcpt_20d"
    rc_oom_read=0
    cmd_20d="INJECT_RECEIPT="$rcpt_20d" INJECT_SFO_FAIL=read_buf $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20d" INJECT_SFO_FAIL=read_buf "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$oom_read_err" || rc_oom_read=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_read" "SFO: out of memory" "$oom_read_err" "Case 20d: read_file malloc OOM" || case_ok=0
    assert_injection_hit "read_buf" "$rcpt_20d" "Case 20d: read_file malloc hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20d_oom_read_buf" "read_file malloc OOM" "$cmd_20d" "$rc_oom_read" "$v" "" "$oom_read_err" "$rcpt_20d"

    # Case 20e: tag_buf malloc failure in parse_xml (exit 1, 'SFO: out of memory', no output file created)
    oom_tag_err="$tmp/oom_tag.err"
    oom_tag_out="$tmp/oom_tag.sfo"
    rm -f "$oom_tag_out"
    rcpt_20e="$tmp/rcpt_20e.log"; rm -f "$rcpt_20e"
    rc_oom_tag=0
    cmd_20e="INJECT_RECEIPT="$rcpt_20e" INJECT_SFO_FAIL=tag_buf $sfo_wrap_bin --fromxml $cand_exported_xml $oom_tag_out"
    INJECT_RECEIPT="$rcpt_20e" INJECT_SFO_FAIL=tag_buf "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$oom_tag_out" >/dev/null 2> "$oom_tag_err" || rc_oom_tag=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_tag" "SFO: out of memory" "$oom_tag_err" "Case 20e: parse_xml tag_buf malloc OOM" || case_ok=0
    assert_injection_hit "tag_buf" "$rcpt_20e" "Case 20e: parse_xml tag_buf hit receipt" || case_ok=0
    if [ -e "$oom_tag_out" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on tag_buf OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20e_oom_tag_buf" "parse_xml tag_buf malloc OOM" "$cmd_20e" "$rc_oom_tag" "$v" "" "$oom_tag_err" "$rcpt_20e"

    # Case 20f: content malloc failure in parse_xml (exit 1, 'SFO: out of memory', no output file created)
    oom_cnt_err="$tmp/oom_cnt.err"
    oom_cnt_out="$tmp/oom_cnt.sfo"
    rm -f "$oom_cnt_out"
    rcpt_20f="$tmp/rcpt_20f.log"; rm -f "$rcpt_20f"
    rc_oom_cnt=0
    cmd_20f="INJECT_RECEIPT="$rcpt_20f" INJECT_SFO_FAIL=content $sfo_wrap_bin --fromxml $cand_exported_xml $oom_cnt_out"
    INJECT_RECEIPT="$rcpt_20f" INJECT_SFO_FAIL=content "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$oom_cnt_out" >/dev/null 2> "$oom_cnt_err" || rc_oom_cnt=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_cnt" "SFO: out of memory" "$oom_cnt_err" "Case 20f: parse_xml content malloc OOM" || case_ok=0
    assert_injection_hit "content" "$rcpt_20f" "Case 20f: parse_xml content hit receipt" || case_ok=0
    if [ -e "$oom_cnt_out" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on content OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20f_oom_content" "parse_xml content malloc OOM" "$cmd_20f" "$rc_oom_cnt" "$v" "" "$oom_cnt_err" "$rcpt_20f"

    # Case 20g: override_title malloc failure in parse_xml (exit 1, 'SFO: out of memory', no output file created)
    oom_ovrt_err="$tmp/oom_ovrt.err"
    oom_ovrt_out="$tmp/oom_ovrt.sfo"
    rm -f "$oom_ovrt_out"
    rcpt_20g="$tmp/rcpt_20g.log"; rm -f "$rcpt_20g"
    trc_20g="$tmp/trc_20g.log"; rm -f "$trc_20g"
    rc_oom_ovrt=0
    title_arg="OVERRIDE_TITLE_LEN_79_TEST_CHARS_0123456789_ABCDEFGHIJKLMNOPQRSTUVWXYZ_EXTRA_PAD"
    cmd_20g="INJECT_TRACE="$trc_20g" INJECT_RECEIPT="$rcpt_20g" INJECT_SFO_FAIL=override_title $sfo_wrap_bin --fromxml $cand_exported_xml $oom_ovrt_out --title=$title_arg"
    INJECT_TRACE="$trc_20g" INJECT_RECEIPT="$rcpt_20g" INJECT_SFO_FAIL=override_title "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$oom_ovrt_out" --title="$title_arg" >/dev/null 2> "$oom_ovrt_err" || rc_oom_ovrt=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_ovrt" "SFO: out of memory" "$oom_ovrt_err" "Case 20g: parse_xml override_title malloc OOM" || case_ok=0
    assert_injection_hit "override_title" "$rcpt_20g" "Case 20g: parse_xml override_title hit receipt" || case_ok=0
    if ! grep -q "^ALLOC 21: malloc" "$trc_20g" || ! grep -q "^ALLOC 22: malloc(count=1, size=81) -> INJECT_FAIL_override_title" "$trc_20g"; then
        row_fail "Row (e)" "Case 20g: wrapper allocation trace did not observe call ordinal 22 of size 81 on pinned XML fixture (see $trc_20g)"
        row_e_ok=0
        case_ok=0
    fi
    if [ -e "$oom_ovrt_out" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on override_title OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20g_oom_override_title" "parse_xml override_title malloc OOM" "$cmd_20g" "$rc_oom_ovrt" "$v" "" "$oom_ovrt_err" "$rcpt_20g" "$trc_20g"

    # Case 20h: override_appid malloc failure in parse_xml (exit 1, 'SFO: out of memory', no output file created)
    oom_ovra_err="$tmp/oom_ovra.err"
    oom_ovra_out="$tmp/oom_ovra.sfo"
    rm -f "$oom_ovra_out"
    rcpt_20h="$tmp/rcpt_20h.log"; rm -f "$rcpt_20h"
    trc_20h="$tmp/trc_20h.log"; rm -f "$trc_20h"
    rc_oom_ovra=0
    appid_arg="OVERRIDE_APPID_LEN_75_TEST_CHARS_0123456789_ABCDEFGHIJKLMNOPQRSTUVWXYZ_EXTRA"
    cmd_20h="INJECT_TRACE="$trc_20h" INJECT_RECEIPT="$rcpt_20h" INJECT_SFO_FAIL=override_appid $sfo_wrap_bin --fromxml $cand_exported_xml $oom_ovra_out --appid=$appid_arg"
    INJECT_TRACE="$trc_20h" INJECT_RECEIPT="$rcpt_20h" INJECT_SFO_FAIL=override_appid "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$oom_ovra_out" --appid="$appid_arg" >/dev/null 2> "$oom_ovra_err" || rc_oom_ovra=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_ovra" "SFO: out of memory" "$oom_ovra_err" "Case 20h: parse_xml override_appid malloc OOM" || case_ok=0
    assert_injection_hit "override_appid" "$rcpt_20h" "Case 20h: parse_xml override_appid hit receipt" || case_ok=0
    if ! grep -q "^ALLOC 23: malloc" "$trc_20h" || ! grep -q "^ALLOC 24: malloc(count=1, size=77) -> INJECT_FAIL_override_appid" "$trc_20h"; then
        row_fail "Row (e)" "Case 20h: wrapper allocation trace did not observe call ordinal 24 of size 77 on pinned XML fixture (see $trc_20h)"
        row_e_ok=0
        case_ok=0
    fi
    if [ -e "$oom_ovra_out" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on override_appid OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20h_oom_override_appid" "parse_xml override_appid malloc OOM" "$cmd_20h" "$rc_oom_ovra" "$v" "" "$oom_ovra_err" "$rcpt_20h" "$trc_20h"

    # Case 20i: offset_key calloc failure in convert_from_xml (exit 1, 'SFO: out of memory', no output file created)
    oom_okey_err="$tmp/oom_okey.err"
    oom_okey_out="$tmp/oom_okey.sfo"
    rm -f "$oom_okey_out"
    rcpt_20i="$tmp/rcpt_20i.log"; rm -f "$rcpt_20i"
    rc_oom_okey=0
    cmd_20i="INJECT_RECEIPT="$rcpt_20i" INJECT_SFO_FAIL=offset_key $sfo_wrap_bin --fromxml $cand_exported_xml $oom_okey_out"
    INJECT_RECEIPT="$rcpt_20i" INJECT_SFO_FAIL=offset_key "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$oom_okey_out" >/dev/null 2> "$oom_okey_err" || rc_oom_okey=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_okey" "SFO: out of memory" "$oom_okey_err" "Case 20i: convert_from_xml offset_key calloc OOM" || case_ok=0
    assert_injection_hit "offset_key" "$rcpt_20i" "Case 20i: convert_from_xml offset_key hit receipt" || case_ok=0
    if [ -e "$oom_okey_out" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on offset_key OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20i_oom_offset_key" "convert_from_xml offset_key calloc OOM" "$cmd_20i" "$rc_oom_okey" "$v" "" "$oom_okey_err" "$rcpt_20i"

    # Case 20j: offset_val calloc failure in convert_from_xml (exit 1, 'SFO: out of memory', no output file created)
    oom_oval_err="$tmp/oom_oval.err"
    oom_oval_out="$tmp/oom_oval.sfo"
    rm -f "$oom_oval_out"
    rcpt_20j="$tmp/rcpt_20j.log"; rm -f "$rcpt_20j"
    rc_oom_oval=0
    cmd_20j="INJECT_RECEIPT="$rcpt_20j" INJECT_SFO_FAIL=offset_val $sfo_wrap_bin --fromxml $cand_exported_xml $oom_oval_out"
    INJECT_RECEIPT="$rcpt_20j" INJECT_SFO_FAIL=offset_val "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$oom_oval_out" >/dev/null 2> "$oom_oval_err" || rc_oom_oval=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_oval" "SFO: out of memory" "$oom_oval_err" "Case 20j: convert_from_xml offset_val calloc OOM" || case_ok=0
    assert_injection_hit "offset_val" "$rcpt_20j" "Case 20j: convert_from_xml offset_val hit receipt" || case_ok=0
    if [ -e "$oom_oval_out" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on offset_val OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20j_oom_offset_val" "convert_from_xml offset_val calloc OOM" "$cmd_20j" "$rc_oom_oval" "$v" "" "$oom_oval_err" "$rcpt_20j"

    # Case 20k: offset_pad calloc failure in convert_from_xml (exit 1, 'SFO: out of memory', no output file created)
    oom_opad_err="$tmp/oom_opad.err"
    oom_opad_out="$tmp/oom_opad.sfo"
    rm -f "$oom_opad_out"
    rcpt_20k="$tmp/rcpt_20k.log"; rm -f "$rcpt_20k"
    rc_oom_opad=0
    cmd_20k="INJECT_RECEIPT="$rcpt_20k" INJECT_SFO_FAIL=offset_pad $sfo_wrap_bin --fromxml $cand_exported_xml $oom_opad_out"
    INJECT_RECEIPT="$rcpt_20k" INJECT_SFO_FAIL=offset_pad "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$oom_opad_out" >/dev/null 2> "$oom_opad_err" || rc_oom_opad=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_opad" "SFO: out of memory" "$oom_opad_err" "Case 20k: convert_from_xml offset_pad calloc OOM" || case_ok=0
    assert_injection_hit "offset_pad" "$rcpt_20k" "Case 20k: convert_from_xml offset_pad hit receipt" || case_ok=0
    if [ -e "$oom_opad_out" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on offset_pad OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20k_oom_offset_pad" "convert_from_xml offset_pad calloc OOM" "$cmd_20k" "$rc_oom_opad" "$v" "" "$oom_opad_err" "$rcpt_20k"

    # Case 20l: offset_len calloc failure in convert_from_xml (exit 1, 'SFO: out of memory', no output file created)
    oom_olen_err="$tmp/oom_olen.err"
    oom_olen_out="$tmp/oom_olen.sfo"
    rm -f "$oom_olen_out"
    rcpt_20l="$tmp/rcpt_20l.log"; rm -f "$rcpt_20l"
    rc_oom_olen=0
    cmd_20l="INJECT_RECEIPT="$rcpt_20l" INJECT_SFO_FAIL=offset_len $sfo_wrap_bin --fromxml $cand_exported_xml $oom_olen_out"
    INJECT_RECEIPT="$rcpt_20l" INJECT_SFO_FAIL=offset_len "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$oom_olen_out" >/dev/null 2> "$oom_olen_err" || rc_oom_olen=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_oom_olen" "SFO: out of memory" "$oom_olen_err" "Case 20l: convert_from_xml offset_len calloc OOM" || case_ok=0
    assert_injection_hit "offset_len" "$rcpt_20l" "Case 20l: convert_from_xml offset_len hit receipt" || case_ok=0
    if [ -e "$oom_olen_out" ]; then
        row_fail "Row (e)" "convert_from_xml created output file on offset_len OOM failure"
        row_e_ok=0
        case_ok=0
    fi
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20l_oom_offset_len" "convert_from_xml offset_len calloc OOM" "$cmd_20l" "$rc_oom_olen" "$v" "" "$oom_olen_err" "$rcpt_20l"

    # Case 20m: read_file fseek(SEEK_END) failure (exit 1, seek error, clean sanitizer stderr)
    io_sk1_err="$tmp/io_sk1.err"
    rcpt_20m="$tmp/rcpt_20m.log"; rm -f "$rcpt_20m"
    rc_io_sk1=0
    cmd_20m="INJECT_RECEIPT="$rcpt_20m" INJECT_SFO_FAIL=seek_first $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20m" INJECT_SFO_FAIL=seek_first "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$io_sk1_err" || rc_io_sk1=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_sk1" "seek error" "$io_sk1_err" "Case 20m: read_file fseek(SEEK_END) failure" || case_ok=0
    assert_injection_hit "seek_first" "$rcpt_20m" "Case 20m: seek_first hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20m_seek_first" "read_file fseek(SEEK_END) failure" "$cmd_20m" "$rc_io_sk1" "$v" "" "$io_sk1_err" "$rcpt_20m"

    # Case 20n: read_file ftell failure (exit 1, tell error, clean sanitizer stderr)
    io_tel_err="$tmp/io_tel.err"
    rcpt_20n="$tmp/rcpt_20n.log"; rm -f "$rcpt_20n"
    rc_io_tel=0
    cmd_20n="INJECT_RECEIPT="$rcpt_20n" INJECT_SFO_FAIL=tell $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20n" INJECT_SFO_FAIL=tell "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$io_tel_err" || rc_io_tel=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_tel" "tell error" "$io_tel_err" "Case 20n: read_file ftell failure" || case_ok=0
    assert_injection_hit "tell" "$rcpt_20n" "Case 20n: tell hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20n_tell" "read_file ftell failure" "$cmd_20n" "$rc_io_tel" "$v" "" "$io_tel_err" "$rcpt_20n"

    # Case 20o: read_file fseek(SEEK_SET) failure (exit 1, seek error, clean sanitizer stderr)
    io_sk2_err="$tmp/io_sk2.err"
    rcpt_20o="$tmp/rcpt_20o.log"; rm -f "$rcpt_20o"
    rc_io_sk2=0
    cmd_20o="INJECT_RECEIPT="$rcpt_20o" INJECT_SFO_FAIL=seek_second $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20o" INJECT_SFO_FAIL=seek_second "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$io_sk2_err" || rc_io_sk2=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_sk2" "seek error" "$io_sk2_err" "Case 20o: read_file fseek(SEEK_SET) failure" || case_ok=0
    assert_injection_hit "seek_second" "$rcpt_20o" "Case 20o: seek_second hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20o_seek_second" "read_file fseek(SEEK_SET) failure" "$cmd_20o" "$rc_io_sk2" "$v" "" "$io_sk2_err" "$rcpt_20o"

    # Case 20p: read_file short fread failure (exit 1, read error, clean sanitizer stderr)
    io_rd_err="$tmp/io_rd.err"
    rcpt_20p="$tmp/rcpt_20p.log"; rm -f "$rcpt_20p"
    rc_io_rd=0
    cmd_20p="INJECT_RECEIPT="$rcpt_20p" INJECT_SFO_FAIL=read_short $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20p" INJECT_SFO_FAIL=read_short "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$io_rd_err" || rc_io_rd=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_rd" "read error" "$io_rd_err" "Case 20p: read_file short fread failure" || case_ok=0
    assert_injection_hit "read_short" "$rcpt_20p" "Case 20p: read_short hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20p_read_short" "read_file short fread failure" "$cmd_20p" "$rc_io_rd" "$v" "" "$io_rd_err" "$rcpt_20p"

    # Case 20q: read_file input fclose failure (exit 1, close error, clean sanitizer stderr)
    io_cls_err="$tmp/io_cls.err"
    rcpt_20q="$tmp/rcpt_20q.log"; rm -f "$rcpt_20q"
    rc_io_cls=0
    cmd_20q="INJECT_RECEIPT="$rcpt_20q" INJECT_SFO_FAIL=input_close $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20q" INJECT_SFO_FAIL=input_close "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$io_cls_err" || rc_io_cls=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_cls" "close error" "$io_cls_err" "Case 20q: read_file input fclose failure" || case_ok=0
    assert_injection_hit "input_close" "$rcpt_20q" "Case 20q: input_close hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20q_input_close" "read_file input fclose failure" "$cmd_20q" "$rc_io_cls" "$v" "" "$io_cls_err" "$rcpt_20q"

    # Case 20r: convert_from_xml output fwrite failure (exit 1, write error, No space left on device)
    io_wr_err="$tmp/io_wr.err"
    io_wr_out="$tmp/io_wr.sfo"
    rm -f "$io_wr_out"
    rcpt_20r="$tmp/rcpt_20r.log"; rm -f "$rcpt_20r"
    rc_io_wr=0
    cmd_20r="INJECT_RECEIPT="$rcpt_20r" INJECT_SFO_FAIL=output_write $sfo_wrap_bin --fromxml $cand_exported_xml $io_wr_out"
    INJECT_RECEIPT="$rcpt_20r" INJECT_SFO_FAIL=output_write "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$io_wr_out" >/dev/null 2> "$io_wr_err" || rc_io_wr=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_wr" "write error" "$io_wr_err" "Case 20r: convert_from_xml output fwrite failure" || case_ok=0
    assert_injection_hit "output_write" "$rcpt_20r" "Case 20r: output_write hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20r_output_write" "convert_from_xml output fwrite failure" "$cmd_20r" "$rc_io_wr" "$v" "" "$io_wr_err" "$rcpt_20r"

    # Case 20s: convert_from_xml output fclose failure (exit 1, close error, Disk quota exceeded)
    io_ocls_err="$tmp/io_ocls.err"
    io_ocls_out="$tmp/io_ocls.sfo"
    rm -f "$io_ocls_out"
    rcpt_20s="$tmp/rcpt_20s.log"; rm -f "$rcpt_20s"
    rc_io_ocls=0
    cmd_20s="INJECT_RECEIPT="$rcpt_20s" INJECT_SFO_FAIL=output_close $sfo_wrap_bin --fromxml $cand_exported_xml $io_ocls_out"
    INJECT_RECEIPT="$rcpt_20s" INJECT_SFO_FAIL=output_close "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$io_ocls_out" >/dev/null 2> "$io_ocls_err" || rc_io_ocls=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_ocls" "close error" "$io_ocls_err" "Case 20s: convert_from_xml output fclose failure" || case_ok=0
    assert_injection_hit "output_close" "$rcpt_20s" "Case 20s: output_close hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20s_output_close" "convert_from_xml output fclose failure" "$cmd_20s" "$rc_io_ocls" "$v" "" "$io_ocls_err" "$rcpt_20s"

    # Case 20t: convert_from_xml write error followed by close error (first-error preservation)
    io_wtc_err="$tmp/io_wtc.err"
    io_wtc_out="$tmp/io_wtc.sfo"
    rm -f "$io_wtc_out"
    rcpt_20t="$tmp/rcpt_20t.log"; rm -f "$rcpt_20t"
    rc_io_wtc=0
    cmd_20t="INJECT_RECEIPT="$rcpt_20t" INJECT_SFO_FAIL=write_then_close $sfo_wrap_bin --fromxml $cand_exported_xml $io_wtc_out"
    INJECT_RECEIPT="$rcpt_20t" INJECT_SFO_FAIL=write_then_close "$sfo_wrap_bin" --fromxml "$cand_exported_xml" "$io_wtc_out" >/dev/null 2> "$io_wtc_err" || rc_io_wtc=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_wtc" "write error" "$io_wtc_err" "Case 20t: first-error preservation (write error)" || case_ok=0
    if ! grep -F "No space left on device" "$io_wtc_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20t: first errno text ('No space left on device') missing from $io_wtc_err"
        row_e_ok=0
        case_ok=0
    fi
    if grep -F "close error" "$io_wtc_err" >/dev/null 2>&1 || grep -F "Input/output error" "$io_wtc_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20t: first-error overwritten by later close error ($io_wtc_err)"
        row_e_ok=0
        case_ok=0
    fi
    assert_injection_hit "output_write" "$rcpt_20t" "Case 20t: output_write hit receipt" || case_ok=0
    assert_injection_hit "output_close" "$rcpt_20t" "Case 20t: output_close hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20t_write_then_close" "convert_from_xml write error followed by close error" "$cmd_20t" "$rc_io_wtc" "$v" "" "$io_wtc_err" "$rcpt_20t"

    # Case 20u: convert_to_xml output fclose failure (exit 1, close error, Disk quota exceeded)
    io_tocls_err="$tmp/io_tocls.err"
    io_tocls_out="$tmp/io_tocls.xml"
    rm -f "$io_tocls_out"
    rcpt_20u="$tmp/rcpt_20u.log"; rm -f "$rcpt_20u"
    rc_io_tocls=0
    cmd_20u="INJECT_RECEIPT="$rcpt_20u" INJECT_SFO_FAIL=output_close $sfo_wrap_bin -t $cand_sfo $io_tocls_out"
    INJECT_RECEIPT="$rcpt_20u" INJECT_SFO_FAIL=output_close "$sfo_wrap_bin" -t "$cand_sfo" "$io_tocls_out" >/dev/null 2> "$io_tocls_err" || rc_io_tocls=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_tocls" "close error" "$io_tocls_err" "Case 20u: convert_to_xml output fclose failure" || case_ok=0
    assert_injection_hit "output_close" "$rcpt_20u" "Case 20u: output_close hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20u_toxml_output_close" "convert_to_xml output fclose failure" "$cmd_20u" "$rc_io_tocls" "$v" "" "$io_tocls_err" "$rcpt_20u"

    # Case 20v: convert_to_xml write error followed by close error (first-error preservation)
    io_twtc_err="$tmp/io_twtc.err"
    io_twtc_out="$tmp/io_twtc.xml"
    rm -f "$io_twtc_out"
    rcpt_20v="$tmp/rcpt_20v.log"; rm -f "$rcpt_20v"
    rc_io_twtc=0
    cmd_20v="INJECT_RECEIPT="$rcpt_20v" INJECT_SFO_FAIL=write_then_close $sfo_wrap_bin -t $cand_sfo $io_twtc_out"
    INJECT_RECEIPT="$rcpt_20v" INJECT_SFO_FAIL=write_then_close "$sfo_wrap_bin" -t "$cand_sfo" "$io_twtc_out" >/dev/null 2> "$io_twtc_err" || rc_io_twtc=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_twtc" "write error" "$io_twtc_err" "Case 20v: convert_to_xml first-error preservation (write error)" || case_ok=0
    if ! grep -F "No space left on device" "$io_twtc_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20v: first errno text ('No space left on device') missing from $io_twtc_err"
        row_e_ok=0
        case_ok=0
    fi
    if grep -F "close error" "$io_twtc_err" >/dev/null 2>&1 || grep -F "Input/output error" "$io_twtc_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20v: first-error overwritten by later close error ($io_twtc_err)"
        row_e_ok=0
        case_ok=0
    fi
    assert_injection_hit "output_write" "$rcpt_20v" "Case 20v: output_write hit receipt" || case_ok=0
    assert_injection_hit "output_close" "$rcpt_20v" "Case 20v: output_close hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20v_toxml_write_then_close" "convert_to_xml write error followed by close error" "$cmd_20v" "$rc_io_twtc" "$v" "" "$io_twtc_err" "$rcpt_20v"

    # Case 20w_normal: stdout normal listing failure (exit 1, 'stdout: write error', no 'Success')
    io_stno_err="$tmp/io_stno.err"
    rcpt_20w="$tmp/rcpt_20w.log"; rm -f "$rcpt_20w"
    rc_io_stno=0
    cmd_20w="INJECT_RECEIPT="$rcpt_20w" INJECT_SFO_FAIL=stdout_no_errno $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20w" INJECT_SFO_FAIL=stdout_no_errno "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$io_stno_err" || rc_io_stno=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_stno" "stdout: write error" "$io_stno_err" "Case 20w: stdout normal listing diagnostic" || case_ok=0
    if grep -F "Success" "$io_stno_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20w: stdout diagnostic contained bogus 'Success' ($io_stno_err)"
        row_e_ok=0
        case_ok=0
    fi
    assert_injection_hit "stdout_no_errno" "$rcpt_20w" "Case 20w: stdout_no_errno hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20w_stdout_normal" "stdout normal listing no-errno failure" "$cmd_20w" "$rc_io_stno" "$v" "" "$io_stno_err" "$rcpt_20w"

    # Case 20w_debug: stdout debug listing failure (exit 1, 'stdout: write error', no 'Success')
    io_stdbg_err="$tmp/io_stdbg.err"
    rcpt_20w_dbg="$tmp/rcpt_20w_dbg.log"; rm -f "$rcpt_20w_dbg"
    rc_io_stdbg=0
    cmd_20w_dbg="INJECT_RECEIPT=\"$rcpt_20w_dbg\" INJECT_SFO_FAIL=stdout_no_errno $sfo_wrap_bin -d -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20w_dbg" INJECT_SFO_FAIL=stdout_no_errno "$sfo_wrap_bin" -d -l "$cand_sfo" >/dev/null 2> "$io_stdbg_err" || rc_io_stdbg=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_stdbg" "stdout: write error" "$io_stdbg_err" "Case 20w_debug: stdout debug listing diagnostic" || case_ok=0
    if grep -F "Success" "$io_stdbg_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20w_debug: stdout diagnostic contained bogus 'Success' ($io_stdbg_err)"
        row_e_ok=0
        case_ok=0
    fi
    assert_injection_hit "stdout_no_errno" "$rcpt_20w_dbg" "Case 20w_debug: stdout_no_errno hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20w_stdout_debug" "stdout debug listing no-errno failure" "$cmd_20w_dbg" "$rc_io_stdbg" "$v" "" "$io_stdbg_err" "$rcpt_20w_dbg"

    # Case 20w_pretty: stdout pretty debug listing failure (exit 1, 'stdout: write error', no 'Success')
    io_stprt_err="$tmp/io_stprt.err"
    rcpt_20w_prt="$tmp/rcpt_20w_prt.log"; rm -f "$rcpt_20w_prt"
    rc_io_stprt=0
    cmd_20w_prt="INJECT_RECEIPT=\"$rcpt_20w_prt\" INJECT_SFO_FAIL=stdout_no_errno $sfo_wrap_bin -d -p -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20w_prt" INJECT_SFO_FAIL=stdout_no_errno "$sfo_wrap_bin" -d -p -l "$cand_sfo" >/dev/null 2> "$io_stprt_err" || rc_io_stprt=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_stprt" "stdout: write error" "$io_stprt_err" "Case 20w_pretty: stdout pretty listing diagnostic" || case_ok=0
    if grep -F "Success" "$io_stprt_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20w_pretty: stdout diagnostic contained bogus 'Success' ($io_stprt_err)"
        row_e_ok=0
        case_ok=0
    fi
    assert_injection_hit "stdout_no_errno" "$rcpt_20w_prt" "Case 20w_pretty: stdout_no_errno hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20w_stdout_pretty" "stdout pretty listing no-errno failure" "$cmd_20w_prt" "$rc_io_stprt" "$v" "" "$io_stprt_err" "$rcpt_20w_prt"

    # Case 20w_stale_before: stale errno prior to stdout write failure (asserts no stale errno reported)
    io_ststale_err="$tmp/io_ststale.err"
    rcpt_20w_stale="$tmp/rcpt_20w_stale.log"; rm -f "$rcpt_20w_stale"
    rc_io_ststale=0
    cmd_20w_stale="INJECT_RECEIPT="$rcpt_20w_stale" INJECT_SFO_FAIL=stdout_stale_before $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20w_stale" INJECT_SFO_FAIL=stdout_stale_before "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$io_ststale_err" || rc_io_ststale=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_ststale" "stdout: write error" "$io_ststale_err" "Case 20w_stale_before: stdout write error fallback" || case_ok=0
    if grep -F "Argument list too long" "$io_ststale_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20w_stale_before: stale errno ('Argument list too long') leaked into diagnostic ($io_ststale_err)"
        row_e_ok=0
        case_ok=0
    fi
    if grep -F "Success" "$io_ststale_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20w_stale_before: diagnostic contained bogus 'Success' ($io_ststale_err)"
        row_e_ok=0
        case_ok=0
    fi
    assert_injection_hit "stale_fclose" "$rcpt_20w_stale" "Case 20w_stale_before: stale_fclose hit receipt" || case_ok=0
    assert_injection_hit "stdout_fail" "$rcpt_20w_stale" "Case 20w_stale_before: stdout_fail hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20w_stdout_stale_before" "stale errno before stdout write failure" "$cmd_20w_stale" "$rc_io_ststale" "$v" "" "$io_ststale_err" "$rcpt_20w_stale"

    # Case 20x: stdout write error followed by flush error (first-error preservation)
    io_stwf_err="$tmp/io_stwf.err"
    rcpt_20x="$tmp/rcpt_20x.log"; rm -f "$rcpt_20x"
    rc_io_stwf=0
    cmd_20x="INJECT_RECEIPT="$rcpt_20x" INJECT_SFO_FAIL=stdout_write_then_flush $sfo_wrap_bin -l $cand_sfo"
    INJECT_RECEIPT="$rcpt_20x" INJECT_SFO_FAIL=stdout_write_then_flush "$sfo_wrap_bin" -l "$cand_sfo" >/dev/null 2> "$io_stwf_err" || rc_io_stwf=$?
    case_ok=1
    judge_sfo_invocation 1 "$rc_io_stwf" "stdout: write error: No space left on device" "$io_stwf_err" "Case 20x: stdout write-then-flush first-error preservation" || case_ok=0
    if grep -F "flush error" "$io_stwf_err" >/dev/null 2>&1 || grep -F "Input/output error" "$io_stwf_err" >/dev/null 2>&1; then
        row_fail "Row (e)" "Case 20x: first-error overwritten by later flush error ($io_stwf_err)"
        row_e_ok=0
        case_ok=0
    fi
    assert_injection_hit "stdout_write" "$rcpt_20x" "Case 20x: stdout_write hit receipt" || case_ok=0
    assert_injection_hit "stdout_flush" "$rcpt_20x" "Case 20x: stdout_flush hit receipt" || case_ok=0
    [ "$case_ok" -eq 1 ] && v="PASS" || v="FAIL"
    save_case_evidence "case_20x_stdout_write_then_flush" "stdout write error followed by flush error" "$cmd_20x" "$rc_io_stwf" "$v" "" "$io_stwf_err" "$rcpt_20x"
fi

if [ -n "$evidence_dir" ]; then
    sha256sum "$repo_root/tools/sfo-pkg/sfo.c" \
              "$golden_dir/input.xml" \
              "$repo_root/tests/sdk/pkg-test.sh" \
              "$expected_sfo" \
              "$expected_list" \
              "$expected_xml" \
              "$expected_roundtrip" > "$evidence_dir/source_input_hashes_after.txt"
    if ! cmp -s "$evidence_dir/source_input_hashes_before.txt" "$evidence_dir/source_input_hashes_after.txt"; then
        row_fail "Row (e)" "Source/input files were modified during Row (e) execution"
        row_e_ok=0
    fi
fi
if [ "$row_e_ok" -eq 1 ]; then
    note "Row (e) PASS: SFO heap allocation, golden baseline parity, CLI 0/1/2 exit status contract, malformed XML cleanups, no-unlink preservation, and deterministic OOM/IO injections verified cleanly with 0 sanitizer defects"
fi

# Final summary
# ---------------------------------------------------------------------------
if [ "$failures" -eq 0 ]; then
    note "ALL ROWS PASSED"
    exit 0
else
    note "$failures ROW(S) FAILED"
    exit 1
fi
