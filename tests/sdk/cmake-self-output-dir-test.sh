#!/usr/bin/env bash
# Test output-directory override support in ps3_add_self and ps3_add_pkg
# (cmake/ps3-self.cmake).
#
# Verifies:
#   1. Default: artifacts land in CMAKE_CURRENT_SOURCE_DIR.
#   2. Keyword override (absolute): ps3_add_self(target OUTPUT_DIRECTORY <dir>)
#      redirects .elf, .self, and .fake.self to <dir>, leaving source clean.
#      ps3_add_pkg follows PS3_SELF_OUTPUT_DIRECTORY property on the target
#      and writes .pkg and .gnpdrm.pkg to <dir>, leaving source clean.
#   3. Variable override (absolute): set(PS3_SELF_OUTPUT_DIRECTORY <dir>)
#      redirects .elf, .self, and .fake.self to <dir>, leaving source clean.
#   4. Relative directory override: ps3_add_self(target OUTPUT_DIRECTORY <rel>)
#      resolves against CMAKE_CURRENT_BINARY_DIR, landing artifacts under the
#      build directory and leaving source clean.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
ps3_self_cmake="${1:-${PS3_SELF_CMAKE_FILE:-$repo_root/cmake/ps3-self.cmake}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

note() {
    printf 'cmake-self-output-dir-test: %s\n' "$*"
}

to_cmake_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mock_ps3dev="$tmp/mock_ps3dev"
mock_bin="$mock_ps3dev/bin"
mkdir -p "$mock_bin"

is_win=0
csc=""
for csc_cand in \
    "/c/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe" \
    "C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe" \
    "/c/Windows/Microsoft.NET/Framework/v4.0.30319/csc.exe"; do
    if [ -f "$csc_cand" ]; then
        csc="$csc_cand"
        is_win=1
        break
    fi
done

if [ "$is_win" -eq 1 ]; then
    note "host is Windows; compiling mock executables with csc.exe"
    cat << 'EOF' > "$tmp/MockTool.cs"
using System;
using System.IO;

class Program {
    static int Main(string[] args) {
        string prog = AppDomain.CurrentDomain.FriendlyName.ToLowerInvariant();
        if (prog.Contains("make_self_npdrm") || prog.Contains("make_self") || prog.Contains("fself")) {
            if (args.Length >= 2) {
                try {
                    string dir = Path.GetDirectoryName(args[1]);
                    if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                    if (File.Exists(args[0])) File.Copy(args[0], args[1], true);
                    else File.WriteAllText(args[1], "mock");
                } catch {}
            }
            return 0;
        }
        if (prog.Contains("strip")) {
            for (int i = 0; i < args.Length - 1; i++) {
                if (args[i] == "-o") {
                    string outPath = args[i + 1];
                    string inPath = args[0];
                    string dir = Path.GetDirectoryName(outPath);
                    if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                    if (File.Exists(inPath)) File.Copy(inPath, outPath, true);
                    else File.WriteAllText(outPath, "mock");
                    return 0;
                }
            }
            return 0;
        }
        if (prog.Contains("pkg")) {
            if (args.Length >= 1) {
                string outPath = args[args.Length - 1];
                string dir = Path.GetDirectoryName(outPath);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                File.WriteAllText(outPath, "mock_pkg");
            }
            return 0;
        }
        if (prog.Contains("sfo")) {
            foreach (var arg in args) {
                if (arg.EndsWith(".SFO", StringComparison.OrdinalIgnoreCase) ||
                    arg.EndsWith(".sfo", StringComparison.OrdinalIgnoreCase)) {
                    string dir = Path.GetDirectoryName(arg);
                    if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                    File.WriteAllText(arg, "mock_sfo");
                }
            }
            return 0;
        }
        return 0;
    }
}
EOF
    out_arg="-out:$(cygpath -w "$mock_bin/mock_tool.exe")"
    src_arg="$(cygpath -w "$tmp/MockTool.cs")"
    MSYS_NO_PATHCONV=1 "$csc" -nologo "$out_arg" "$src_arg"
    for tool in make_self.exe fself.exe sprxlinker.exe make_self_npdrm.exe sfo.exe pkg.exe package_finalize.exe mock_strip.exe; do
        cp "$mock_bin/mock_tool.exe" "$mock_bin/$tool"
    done
    mock_strip="$mock_bin/mock_strip.exe"
else
    note "host is Unix; creating shell script mock tools"
    cat << 'EOF' > "$mock_bin/sprxlinker"
#!/bin/sh
exit 0
EOF
    cat << 'EOF' > "$mock_bin/make_self"
#!/bin/sh
mkdir -p "$(dirname "$2")"
cp "$1" "$2"
EOF
    cat << 'EOF' > "$mock_bin/fself"
#!/bin/sh
mkdir -p "$(dirname "$2")"
cp "$1" "$2"
EOF
    cat << 'EOF' > "$mock_bin/make_self_npdrm"
#!/bin/sh
mkdir -p "$(dirname "$2")"
cp "$1" "$2"
EOF
    cat << 'EOF' > "$mock_bin/sfo"
#!/bin/sh
for arg in "$@"; do
    case "$arg" in
        *.SFO|*.sfo)
            mkdir -p "$(dirname "$arg")"
            touch "$arg"
            ;;
    esac
done
EOF
    cat << 'EOF' > "$mock_bin/pkg"
#!/bin/sh
eval out=\${$#}
mkdir -p "$(dirname "$out")"
touch "$out"
EOF
    cat << 'EOF' > "$mock_bin/package_finalize"
#!/bin/sh
exit 0
EOF
    cat << 'EOF' > "$mock_bin/mock_strip"
#!/bin/sh
in=""
out=""
while [ "$#" -gt 0 ]; do
    if [ "$1" = "-o" ] && [ "$#" -ge 2 ]; then
        out="$2"
        shift 2
    else
        in="$1"
        shift
    fi
done
mkdir -p "$(dirname "$out")"
cp "$in" "$out"
EOF
    chmod +x "$mock_bin"/*
    mock_strip="$mock_bin/mock_strip"
fi

cmake_mock_ps3dev="$(to_cmake_path "$mock_ps3dev")"
cmake_mock_strip="$(to_cmake_path "$mock_strip")"
cmake_ps3_self="$(to_cmake_path "$ps3_self_cmake")"

cmake_icon="$(to_cmake_path "$repo_root/sdk/assets/ICON0.PNG")"
cmake_sfo_xml="$(to_cmake_path "$repo_root/cmake/templates/sfo.xml")"

run_sample() {
    local mode="$1"
    local sample_dir="$2"
    local out_dir="$3"
    local bld_dir="$4"

    mkdir -p "$sample_dir" "$bld_dir"
    cat << 'EOF' > "$sample_dir/main.c"
int main(void) { return 0; }
EOF

    local cmake_out_dir=""
    if [ -n "$out_dir" ]; then
        if [ "$mode" = "relative" ]; then
            cmake_out_dir="$out_dir"
        else
            cmake_out_dir="$(to_cmake_path "$out_dir")"
        fi
    fi

    cat << EOF > "$sample_dir/CMakeLists.txt"
cmake_minimum_required(VERSION 3.20)
project(test_sample C)

set(_PS3_SELF_SDK_INSTALL_PROBED TRUE CACHE BOOL "" FORCE)
set(CMAKE_STRIP "$cmake_mock_strip" CACHE FILEPATH "" FORCE)

include("$cmake_ps3_self")

add_executable(sample_app main.c)

if(TEST_CASE STREQUAL "keyword" OR TEST_CASE STREQUAL "relative")
    ps3_add_self(sample_app OUTPUT_DIRECTORY "$cmake_out_dir")
    ps3_add_pkg(sample_app
        CONTENTID "UP0001-TEST12345_00-0000000000000000"
        ICON "$cmake_icon"
        SFOXML "$cmake_sfo_xml")
elseif(TEST_CASE STREQUAL "variable")
    set(PS3_SELF_OUTPUT_DIRECTORY "$cmake_out_dir")
    ps3_add_self(sample_app)
else()
    ps3_add_self(sample_app)
endif()
EOF

    if ! cmake -S "$sample_dir" -B "$bld_dir" \
        -DTEST_CASE="$mode" \
        -DPS3DEV="$cmake_mock_ps3dev" > "$bld_dir/configure.log" 2>&1; then
        cat "$bld_dir/configure.log" >&2
        fail "cmake configure failed for $mode in $sample_dir"
    fi
    if ! cmake --build "$bld_dir" > "$bld_dir/build.log" 2>&1; then
        cat "$bld_dir/build.log" >&2
        fail "cmake build failed for $mode in $sample_dir"
    fi
}

# -----------------------------------------------------------------------------
# Case 1: Default behavior (artifacts land in source dir)
# -----------------------------------------------------------------------------
note "step 1: testing default output location (source dir)"
c1_src="$tmp/c1_src"
c1_bld="$tmp/c1_bld"
run_sample "default" "$c1_src" "" "$c1_bld"

[ -f "$c1_src/sample_app.elf" ]       || fail "case 1: sample_app.elf missing from source dir"
[ -f "$c1_src/sample_app.self" ]      || fail "case 1: sample_app.self missing from source dir"
[ -f "$c1_src/sample_app.fake.self" ] || fail "case 1: sample_app.fake.self missing from source dir"
note "case 1 PASS: default builds write artifacts into source dir"

# -----------------------------------------------------------------------------
# Case 2: Keyword override (OUTPUT_DIRECTORY <dir>) with absolute path
# -----------------------------------------------------------------------------
note "step 2: testing absolute keyword OUTPUT_DIRECTORY override and ps3_add_pkg propagation"
c2_src="$tmp/c2_src"
c2_out="$tmp/c2_out"
c2_bld="$tmp/c2_bld"
run_sample "keyword" "$c2_src" "$c2_out" "$c2_bld"

[ -f "$c2_out/sample_app.elf" ]        || fail "case 2: sample_app.elf missing from custom out dir"
[ -f "$c2_out/sample_app.self" ]       || fail "case 2: sample_app.self missing from custom out dir"
[ -f "$c2_out/sample_app.fake.self" ]  || fail "case 2: sample_app.fake.self missing from custom out dir"
[ -f "$c2_out/sample_app.pkg" ]        || fail "case 2: sample_app.pkg missing from custom out dir"
[ -f "$c2_out/sample_app.gnpdrm.pkg" ] || fail "case 2: sample_app.gnpdrm.pkg missing from custom out dir"

[ ! -f "$c2_src/sample_app.elf" ]        || fail "case 2: sample_app.elf leaked into source dir"
[ ! -f "$c2_src/sample_app.self" ]       || fail "case 2: sample_app.self leaked into source dir"
[ ! -f "$c2_src/sample_app.fake.self" ]  || fail "case 2: sample_app.fake.self leaked into source dir"
[ ! -f "$c2_src/sample_app.pkg" ]        || fail "case 2: sample_app.pkg leaked into source dir"
[ ! -f "$c2_src/sample_app.gnpdrm.pkg" ] || fail "case 2: sample_app.gnpdrm.pkg leaked into source dir"
note "case 2 PASS: keyword OUTPUT_DIRECTORY redirects all .elf/.self/.fake.self and .pkg/.gnpdrm.pkg"

# -----------------------------------------------------------------------------
# Case 3: Variable override (PS3_SELF_OUTPUT_DIRECTORY) with absolute path
# -----------------------------------------------------------------------------
note "step 3: testing absolute variable PS3_SELF_OUTPUT_DIRECTORY override"
c3_src="$tmp/c3_src"
c3_out="$tmp/c3_out"
c3_bld="$tmp/c3_bld"
run_sample "variable" "$c3_src" "$c3_out" "$c3_bld"

[ -f "$c3_out/sample_app.elf" ]       || fail "case 3: sample_app.elf missing from custom out dir"
[ -f "$c3_out/sample_app.self" ]      || fail "case 3: sample_app.self missing from custom out dir"
[ -f "$c3_out/sample_app.fake.self" ] || fail "case 3: sample_app.fake.self missing from custom out dir"

[ ! -f "$c3_src/sample_app.elf" ]       || fail "case 3: sample_app.elf leaked into source dir"
[ ! -f "$c3_src/sample_app.self" ]      || fail "case 3: sample_app.self leaked into source dir"
[ ! -f "$c3_src/sample_app.fake.self" ] || fail "case 3: sample_app.fake.self leaked into source dir"
note "case 3 PASS: variable PS3_SELF_OUTPUT_DIRECTORY redirects .elf/.self/.fake.self"

# -----------------------------------------------------------------------------
# Case 4: Relative directory override (resolves to CMAKE_CURRENT_BINARY_DIR)
# -----------------------------------------------------------------------------
note "step 4: testing relative OUTPUT_DIRECTORY override under build directory"
c4_src="$tmp/c4_src"
c4_bld="$tmp/c4_bld"
c4_out="$c4_bld/rel_out"
run_sample "relative" "$c4_src" "rel_out" "$c4_bld"

[ -f "$c4_out/sample_app.elf" ]        || fail "case 4: sample_app.elf missing from relative out dir under build"
[ -f "$c4_out/sample_app.self" ]       || fail "case 4: sample_app.self missing from relative out dir under build"
[ -f "$c4_out/sample_app.fake.self" ]  || fail "case 4: sample_app.fake.self missing from relative out dir under build"
[ -f "$c4_out/sample_app.pkg" ]        || fail "case 4: sample_app.pkg missing from relative out dir under build"
[ -f "$c4_out/sample_app.gnpdrm.pkg" ] || fail "case 4: sample_app.gnpdrm.pkg missing from relative out dir under build"

[ ! -f "$c4_src/sample_app.elf" ]        || fail "case 4: sample_app.elf leaked into source dir"
[ ! -f "$c4_src/sample_app.self" ]       || fail "case 4: sample_app.self leaked into source dir"
[ ! -f "$c4_src/sample_app.fake.self" ]  || fail "case 4: sample_app.fake.self leaked into source dir"
[ ! -f "$c4_src/sample_app.pkg" ]        || fail "case 4: sample_app.pkg leaked into source dir"
[ ! -f "$c4_src/sample_app.gnpdrm.pkg" ] || fail "case 4: sample_app.gnpdrm.pkg leaked into source dir"
[ ! -d "$c4_src/rel_out" ]               || fail "case 4: rel_out directory created under source dir instead of build dir"
note "case 4 PASS: relative OUTPUT_DIRECTORY resolves under build dir and leaves source dir clean"

note "ALL CHECKS PASSED"
