#!/usr/bin/env bash
# ps3_add_spu_image refuses a SPURS job image that has no _start.
#
# NOSTARTFILES with -mspurs-job and no job library used to link clean: the
# job startup was never pulled in, --gc-sections dropped cellSpursJobMain2
# and a 16-byte job shipped, faulting on its first dispatch.  JOBBIN and
# JOBBIN_WRAP images now link with --require-defined=_start.
#
# Needs an installed SDK (PS3DEV, PS3DK) with the SPU toolchain on PATH.
#   bad:    NOSTARTFILES JOBBIN -mspurs-job, no LIBS   -> link fails: _start required
#   driver: JOBBIN -mspurs-job                          -> builds, image has code
#   manual: NOSTARTFILES JOBBIN + spurs_job.ld + LIBS spurs_job -> builds
# Optional $1: the ps3-self.cmake to test (default: this tree's).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
ps3_self_cmake="${1:-$repo_root/cmake/ps3-self.cmake}"
: "${PS3DEV:?PS3DEV must point at an installed SDK}"
: "${PS3DK:?PS3DK must point at an installed SDK}"

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
note() { printf 'spu-jobbin-entry-guard-test: %s\n' "$*"; }

tmp=$(mktemp -d)
[ -n "${KEEP_TMP:-}" ] || trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/job.c" << 'EOF'
#include <stdint.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_chain.h>
#include <cell/spurs/job_context.h>
__asm__(".pushsection .data\n.global _cell_spu_ls_param\n.align 4\n"
        "_cell_spu_ls_param:\n.long 0x100, 0x1000, 0, 0\n.popsection\n"
        ".pushsection .cell_spu_ls_param\n.long 0x100, 0x1000\n.popsection\n");
void cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    __attribute__((aligned(16))) uint32_t buf[4] = { 1, 2, 3, 4 };
    mfc_put(buf, job->workArea.userData[0], sizeof buf, ctx->dmaTag, 0, 0);
}
EOF
echo 'int main(void) { return 0; }' > "$tmp/main.c"

run_case() {
    local name="$1" spu_args="$2"
    local src="$tmp/$name" bld="$tmp/$name-build"
    mkdir -p "$src"
    cat > "$src/CMakeLists.txt" << EOF
cmake_minimum_required(VERSION 3.20)
project(guard_$name C CXX ASM)
set(_PS3_SELF_SDK_INSTALL_PROBED TRUE CACHE BOOL "" FORCE)
include("$ps3_self_cmake")
add_executable(app "$tmp/main.c")
ps3_add_spu_image(app NAME job SOURCES "$tmp/job.c" JOBBIN $spu_args)
EOF
    cmake -S "$src" -B "$bld" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$repo_root/cmake/ps3-ppu-toolchain.cmake" \
        > "$bld.configure.log" 2>&1 || { cat "$bld.configure.log" >&2; fail "$name: configure failed"; }
    cmake --build "$bld" > "$bld.build.log" 2>&1
}

note "bad: NOSTARTFILES -mspurs-job without a job library"
if run_case bad "NOSTARTFILES CFLAGS -mspurs-job LDFLAGS -mspurs-job -Wl,-q"; then
    fail "bad: an image with no _start linked"
fi
grep -qE "required symbol .?_start.? not defined" "$tmp/bad-build.build.log" || { cat "$tmp/bad-build.build.log" >&2; fail "bad: failure is not the missing _start"; }
note "bad PASS: refused, naming _start"

note "driver: -mspurs-job"
run_case driver "CFLAGS -mspurs-job LDFLAGS -mspurs-job -Wl,-q" \
    || { tail -20 "$tmp/driver-build.build.log" >&2; fail "driver: build failed"; }
elf=$(find "$tmp/driver-build" -path '*/spu/job/job.bin' | head -1)
spu-elf-nm "$elf" | grep -q ' T cellSpursJobMain2$' || fail "driver: cellSpursJobMain2 missing from $elf"
note "driver PASS"

note "manual: NOSTARTFILES + spurs_job.ld + libspurs_job"
run_case manual "NOSTARTFILES CFLAGS -mspurs-job LDFLAGS -mspurs-job -Wl,-q LDSCRIPT $PS3DK/spu/ldscripts/spurs_job.ld LIBS spurs_job" \
    || { tail -20 "$tmp/manual-build.build.log" >&2; fail "manual: build failed"; }
note "manual PASS"
note "ALL CHECKS PASSED"
