#!/usr/bin/env bash
# librt's pthread_mutex_trylock returns EBUSY BY NAME: the value <errno.h> gives EBUSY on the target.
#
# Since newlib patch 0020 the E* names on the PS3 LV2 target are the Lv-2 status codes, so EBUSY is 0x8001000A
# and a trylock on a held mutex must return exactly that; before 0020 (or with GCC 12.4's runtime) EBUSY is the
# POSIX 16.  Either way the function must return the value a caller compares against BY NAME - the bug this test
# was written for was pthread.c returning one value while <errno.h> gave callers the other.
#
# Preprocesses EBUSY with the target compiler (the installed headers decide), then disassembles
# pthread_mutex_trylock in an installed librt.a and requires that it materialises THAT value and never the other
# one (0x8001000A = lis rN,-32767 then ori rN,rN,10; 16 = li rN,16).
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

# EBUSY as the target's <errno.h> defines it
printf '#include <errno.h>\nEBUSY_IS EBUSY\n' > "$tmp/e.c"
pp=$("$bindir/powerpc64-ps3-elf-gcc" -E -P "$tmp/e.c") || fail "preprocessing <errno.h> failed"
val=$(sed -n 's/^EBUSY_IS *//p' <<<"$pp" | tr -d ' ()')
case "$val" in
    16) want=posix ;;
    -2147418102|0x8001000A|0x8001000a) want=lv2 ;;
    *) fail "unexpected EBUSY value from <errno.h>: '$val'" ;;
esac

(cd "$tmp" && "$bindir/powerpc64-ps3-elf-ar" x "$librt" pthread.o) || fail "librt.a has no pthread.o"
"$bindir/powerpc64-ps3-elf-objdump" -d -j .text.pthread_mutex_trylock "$tmp/pthread.o" > "$tmp/trylock.s" 2> /dev/null \
    || fail "pthread.o has no .text.pthread_mutex_trylock"
grep -q 'blr' "$tmp/trylock.s" || fail "pthread_mutex_trylock is empty in pthread.o"

# A register loaded with lis N,-32767 (0x8001xxxx) and then or-ed with 10.
lv2=$(awk '
    $0 ~ /lis[ \t]+r[0-9]+,-32767$/ { split($NF, a, ","); hi[a[1]] = 1 }
    $0 ~ /ori[ \t]+r[0-9]+,r[0-9]+,10$/ { split($NF, a, ","); if (hi[a[2]]) print }
' "$tmp/trylock.s")
posix=$(grep -E 'li[[:space:]]+r[0-9]+,16$' "$tmp/trylock.s" || true)
if [ $want = lv2 ]; then
    [ -n "$lv2" ] || fail "<errno.h> EBUSY is the Lv-2 0x8001000A, but pthread_mutex_trylock never materialises it"
    [ -z "$posix" ] || fail "<errno.h> EBUSY is the Lv-2 0x8001000A, but pthread_mutex_trylock loads POSIX 16: $posix"
else
    [ -n "$posix" ] || fail "<errno.h> EBUSY is POSIX 16, but pthread_mutex_trylock never loads 16"
    [ -z "$lv2" ] || fail "<errno.h> EBUSY is POSIX 16, but pthread_mutex_trylock returns the Lv-2 0x8001000A: $lv2"
fi
note "PASS ($librt: EBUSY by name = $val, $want)"
