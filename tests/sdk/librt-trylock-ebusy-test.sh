#!/usr/bin/env bash
# librt's pthread_mutex_trylock returns POSIX EBUSY (16), not the Lv-2 code.
#
# pthread.c includes <sys/mutex.h>, which reaches <sys/synchronization.h>;
# that header redefines EBUSY to the Lv-2 value 0x8001000A.  A trylock on a
# held mutex then returned 0x8001000A, and POSIX callers comparing the
# result against EBUSY from <errno.h> saw an unknown error.
#
# Disassembles pthread_mutex_trylock in an installed librt.a and requires
# that it never materialises 0x8001000A (lis rN,-32767 then ori rN,rN,10)
# and does load 16.
# Optional $1: the librt.a to test (default: $PS3DK/ppu/lib/librt.a).
set -euo pipefail

if [ -z "${PS3DK:-}" ] && [ -z "${1:-}" ]; then
    echo "librt-trylock-ebusy: SKIP (set PS3DK to an installed SDK or pass a librt.a)"
    exit 0
fi

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
note() { printf 'librt-trylock-ebusy-test: %s\n' "$*"; }

librt="${1:-$PS3DK/ppu/lib/librt.a}"
[ -f "$librt" ] || fail "no librt.a at $librt"
librt="$(cd "$(dirname "$librt")" && pwd -P)/$(basename "$librt")"

bindir=""
for d in "${PS3DEV:-}/ppu/bin" "${PS3DK:-}/ppu/bin"; do
    if [ -x "$d/powerpc64-ps3-elf-objdump" ] || [ -x "$d/powerpc64-ps3-elf-objdump.exe" ]; then
        bindir="$d"; break
    fi
done
if [ -z "$bindir" ]; then
    command -v powerpc64-ps3-elf-objdump > /dev/null || fail "powerpc64-ps3-elf-objdump not found"
    bindir="$(dirname "$(command -v powerpc64-ps3-elf-objdump)")"
fi

tmp=$(mktemp -d)
[ -n "${KEEP_TMP:-}" ] || trap 'rm -rf "$tmp"' EXIT

(cd "$tmp" && "$bindir/powerpc64-ps3-elf-ar" x "$librt" pthread.o) || fail "librt.a has no pthread.o"
"$bindir/powerpc64-ps3-elf-objdump" -d -j .text.pthread_mutex_trylock "$tmp/pthread.o" > "$tmp/trylock.s" 2> /dev/null \
    || fail "pthread.o has no .text.pthread_mutex_trylock"
grep -q 'blr' "$tmp/trylock.s" || fail "pthread_mutex_trylock is empty in pthread.o"

# A register loaded with lis N,-32767 (0x8001xxxx) and then or-ed with 10.
lv2=$(awk '
    $0 ~ /lis[ \t]+r[0-9]+,-32767$/ { split($NF, a, ","); hi[a[1]] = 1 }
    $0 ~ /ori[ \t]+r[0-9]+,r[0-9]+,10$/ { split($NF, a, ","); if (hi[a[2]]) print }
' "$tmp/trylock.s")
[ -z "$lv2" ] || fail "pthread_mutex_trylock returns the Lv-2 EBUSY 0x8001000A: $lv2"
grep -Eq 'li[[:space:]]+r[0-9]+,16$' "$tmp/trylock.s" \
    || fail "pthread_mutex_trylock never loads POSIX EBUSY (16)"

note "PASS ($librt)"
