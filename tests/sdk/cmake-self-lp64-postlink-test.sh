#!/usr/bin/env bash
# ps3_add_self must pass --lp64 to sprxlinker for every LP64 executable.
#
# An LP64 SELF needs `sprxlinker --lp64` so import call sites get their TOC
# restore. The LP64 toolchain file sets PS3_SPRXLINKER_FLAGS=--lp64 for the
# whole project; a target that opts into LP64 itself (-mlp64 in its link
# options under the ILP32 toolchain file, as samples that build both ABIs do)
# used to be post-linked without it.
#
# The test configures a project with mock tools and reads the generated
# post-build sprxlinker command of each target (no PPU toolchain needed):
#   ILP32 target                        -> no --lp64
#   -mlp64 target                       -> --lp64 once
#   PS3_SPRXLINKER_FLAGS=--lp64 project -> --lp64 once (not duplicated)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
ps3_self_cmake="${1:-$repo_root/cmake/ps3-self.cmake}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mock_bin="$tmp/ps3dev/bin"
mkdir -p "$mock_bin"
for tool in sprxlinker make_self fself make_self_npdrm sfo pkg package_finalize strip; do
    printf '#!/bin/sh\nexit 0\n' > "$mock_bin/$tool"
    chmod +x "$mock_bin/$tool"
done

configure() {
    local dir="$1" global_flags="$2"
    mkdir -p "$dir/src"
    printf 'int main(void) { return 0; }\n' > "$dir/src/main.c"
    cat << EOF > "$dir/src/CMakeLists.txt"
cmake_minimum_required(VERSION 3.20)
project(lp64_postlink C)
set(_PS3_SELF_SDK_INSTALL_PROBED TRUE CACHE BOOL "" FORCE)
set(CMAKE_STRIP "$mock_bin/strip" CACHE FILEPATH "" FORCE)
set(PS3_SPRXLINKER_FLAGS "$global_flags" CACHE STRING "" FORCE)
include("$ps3_self_cmake")
add_executable(app_ilp32 main.c)
ps3_add_self(app_ilp32)
add_executable(app_lp64 main.c)
target_link_options(app_lp64 PRIVATE -mlp64)
ps3_add_self(app_lp64)
EOF
    if ! cmake -S "$dir/src" -B "$dir/build" -G "Unix Makefiles" \
            -DPS3DEV="$tmp/ps3dev" > "$dir/configure.log" 2>&1; then
        cat "$dir/configure.log" >&2
        fail "configure failed ($dir)"
    fi
}

# Count --lp64 on the sprxlinker line of one target's generated build rules.
lp64_count() {
    local dir="$1" target="$2"
    local rules="$dir/build/CMakeFiles/$target.dir/build.make"
    [ -f "$rules" ] || fail "no generated rules for $target: $rules"
    local line
    line="$(grep -E '/sprxlinker( |")' "$rules" || true)"
    [ -n "$line" ] || fail "no sprxlinker command generated for $target"
    # An empty argument ("") makes the real sprxlinker fail.
    case "$line" in
        *'""'*|*"''"*) fail "sprxlinker command for $target has an empty argument: $line" ;;
    esac
    # grep exits 1 when it finds nothing; zero matches is a valid count here.
    { printf '%s\n' "$line" | grep -o -- '--lp64' || true; } | wc -l | tr -d ' '
}

configure "$tmp/ilp32tc" ""
# Assignments (not tests) so a failure inside lp64_count stops the script
# with its own message.
n="$(lp64_count "$tmp/ilp32tc" app_ilp32)"
[ "$n" = 0 ] || fail "ILP32 target was post-linked with --lp64"
n="$(lp64_count "$tmp/ilp32tc" app_lp64)"
[ "$n" = 1 ] || fail "-mlp64 target was not post-linked with exactly one --lp64"

configure "$tmp/lp64tc" "--lp64"
n="$(lp64_count "$tmp/lp64tc" app_lp64)"
[ "$n" = 1 ] || fail "--lp64 duplicated or missing under PS3_SPRXLINKER_FLAGS=--lp64"
n="$(lp64_count "$tmp/lp64tc" app_ilp32)"
[ "$n" = 1 ] || fail "LP64 toolchain flags no longer reach every target"

echo "cmake-self-lp64-postlink: PASS"
