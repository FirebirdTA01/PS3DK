#!/usr/bin/env bash
# The SDK installs an SPU libatomic.a with one external definition of each
# cellAtomic* operation, so SPU code that links -latomic, declares the
# functions itself, or takes their addresses links.  <cell/atomic.h> keeps
# the operations inline, and a program including it must still link with
# -latomic present (no duplicate definitions).
#
# usage: spu-libatomic-test.sh [--ps3dev DIR]
#   the archive and headers come from $PS3DK (default DIR/ps3dk)
set -u
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "spu-libatomic: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
sdk="${PS3DK:-$ps3dev/ps3dk}"
cc="$ps3dev/spu/bin/spu-elf-gcc"
nm="$ps3dev/spu/bin/spu-elf-nm"
status=0
fail() { echo "spu-libatomic: FAIL: $*"; status=1; }
ok() { echo "spu-libatomic: ok   $*"; }
[ -x "$cc" ] && [ -x "$nm" ] || { fail "no SPU compiler under $ps3dev"; exit 1; }
lib="$sdk/spu/lib/libatomic.a"
[ -f "$lib" ] || { fail "no $lib"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

ops32="Add32 Sub32 And32 Or32 Store32 Incr32 Decr32 Nop32 TestAndDecr32 CompareAndSwap32"
ops64="Add64 Sub64 And64 Or64 Store64 Incr64 Decr64 Nop64 TestAndDecr64 CompareAndSwap64"

# One defined text symbol per operation, each in its own member.
"$nm" "$lib" > "$work/nm.txt" 2>&1
for op in $ops32 $ops64; do
    n=$(grep -c " T cellAtomic$op\$" "$work/nm.txt")
    [ "$n" = 1 ] && ok "defines cellAtomic$op" || fail "cellAtomic$op defined $n times"
done
members=$(grep -c ':$' "$work/nm.txt")
[ "$members" = 20 ] && ok "20 members" || fail "$members members, expected 20"

# A program that declares the functions itself (no header) and takes every
# address must resolve all twenty from the archive.
{
    echo '#include <stdint.h>'
    for op in Add Sub And Or Store; do
        echo "uint32_t cellAtomic${op}32(uint32_t *, uint64_t, uint32_t);"
        echo "uint64_t cellAtomic${op}64(uint64_t *, uint64_t, uint64_t);"
    done
    for op in Incr Decr Nop TestAndDecr; do
        echo "uint32_t cellAtomic${op}32(uint32_t *, uint64_t);"
        echo "uint64_t cellAtomic${op}64(uint64_t *, uint64_t);"
    done
    echo 'uint32_t cellAtomicCompareAndSwap32(uint32_t *, uint64_t, uint32_t, uint32_t);'
    echo 'uint64_t cellAtomicCompareAndSwap64(uint64_t *, uint64_t, uint64_t, uint64_t);'
    echo 'void *const table[] = {'
    for op in $ops32 $ops64; do echo "    (void *)cellAtomic$op,"; done
    echo '};'
    echo 'int main(void) { return table[0] == 0; }'
} > "$work/decl.c"
if "$cc" -O2 -Wall -Werror "$work/decl.c" -L"$sdk/spu/lib" -latomic -o "$work/decl.elf" > "$work/decl.log" 2>&1; then
    n=$("$nm" "$work/decl.elf" | grep -c " T cellAtomic")
    [ "$n" = 20 ] && ok "declared-only program links all 20" || fail "linked program has $n cellAtomic symbols"
else
    fail "declared-only program: $(grep -m1 -E 'undefined|error' "$work/decl.log")"
fi

# The header's inlines and the archive coexist: no duplicate definitions.
cat > "$work/inline.c" <<'EOF'
#include <cell/atomic.h>
static uint32_t line[32] __attribute__((aligned(128)));
uint32_t bump(uint64_t ea) { return cellAtomicAdd32(line, ea, 1); }
int main(void) { return 0; }
EOF
if "$cc" -O2 -Wall -Werror -I"$sdk/spu/include" "$work/inline.c" -L"$sdk/spu/lib" -latomic -o "$work/inline.elf" > "$work/inline.log" 2>&1; then
    ok "header inlines link beside -latomic"
else
    fail "header program: $(grep -m1 -E 'error|multiple' "$work/inline.log")"
fi

[ "$status" -eq 0 ] && echo "spu-libatomic: PASS"
exit $status
