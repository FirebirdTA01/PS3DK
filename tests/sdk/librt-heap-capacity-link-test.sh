#!/usr/bin/env bash
# Link checks for __ps3tc_heap_capacity against built librt archives, both ABIs.
#   LIBRT_DIR      directory holding librt.a (ILP32) and lp64/librt.a (default $PS3DK/ppu/lib)
#   OLD_LIBRT_DIR  optional: a runtime that predates the query (control: the weak address must be null)
#   PPU_CC         PPU compiler (default $PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc)
# Skips (exit 0) without a PPU compiler or a librt directory.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd -P)
cc=${PPU_CC:-${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf-gcc}
new_dir=${LIBRT_DIR:-${PS3DK:-}/ppu/lib}
old_dir=${OLD_LIBRT_DIR:-}
if [[ ! -x $cc || ! -f $new_dir/librt.a ]]; then
    echo "librt heap capacity link test: SKIP (no PPU compiler or librt.a)"
    exit 0
fi
nm=${cc%gcc}nm
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failed=0
report() { if [[ $2 == 0 ]]; then echo "heap capacity $1: PASS"; else echo "heap capacity $1: FAIL"; failed=1; fi; }

# The application-side idiom: a weak declaration, never the header prototype.
cat > "$work/weak.c" <<'SRC'
#include <stdint.h>
#include <stdlib.h>
extern uint64_t __ps3tc_heap_capacity(void) __attribute__((weak));
volatile uintptr_t heap_capacity_address;
int main(void)
{
    void *block = malloc(16);
    heap_capacity_address = (uintptr_t)&__ps3tc_heap_capacity;
    return block != NULL;
}
SRC
# The public macro and header prototype, with a strong reference.
cat > "$work/strong.c" <<'SRC'
#include <stdint.h>
#include <stdlib.h>
#include <sys/heap_config.h>
PS3TC_HEAP_SIZE(UINT64_C(128) * 1024 * 1024);
volatile uint64_t heap_capacity_value;
int main(void)
{
    heap_capacity_value = __ps3tc_heap_capacity();
    return malloc(16) == NULL;
}
SRC

link_case() {   # name abi-flag library-dir source
    local name=$1 flag=$2 dir=$3 src=$4
    # shellcheck disable=SC2086
    "$cc" $flag -O2 -I"$root/sdk/include" -L"$dir" -Wl,-Map,"$work/$name.map" \
        "$work/$src" -o "$work/$name.elf" -lrt > "$work/$name.log" 2>&1 || { cat "$work/$name.log" >&2; return 1; }
    # the archive that supplied sbrk.o is the one named by the caller
    grep -qF "$dir/librt.a(sbrk.o)" "$work/$name.map"
}
defined() { "$nm" "$1" | grep -Eq "^[0-9a-f]+ [A-Za-z] \.?__ps3tc_heap_capacity$"; }
# ld drops an unresolved weak reference from a static link (the address is 0), so the symbol must not be defined.

for abi in ilp32 lp64; do
    flag=""; sub=""
    if [[ $abi == lp64 ]]; then flag=-mlp64; sub=/lp64; fi
    new=$new_dir$sub
    # the archive exports the symbol from sbrk.o
    if "$nm" -A "$new/librt.a" 2>/dev/null | grep -Eq 'sbrk\.o:[0-9a-f]+ [TtDd] \.?__ps3tc_heap_capacity$'; then rc=0; else rc=1; fi
    report "$abi librt.a exports the symbol from sbrk.o" $rc
    # strong reference links
    if link_case "strong-$abi" "$flag" "$new" strong.c; then rc=0; else rc=1; fi
    report "$abi strong reference links against the new librt" $rc
    # the weak declaration resolves to a real address
    if link_case "weak-new-$abi" "$flag" "$new" weak.c && defined "$work/weak-new-$abi.elf"; then rc=0; else rc=1; fi
    report "$abi weak address non-null against the new librt" $rc
    if [[ -n $old_dir ]]; then
        # control: an older runtime has no definition, so the weak address is null
        if link_case "weak-old-$abi" "$flag" "$old_dir$sub" weak.c && ! defined "$work/weak-old-$abi.elf"; then rc=0; else rc=1; fi
        report "$abi weak address null against the older librt" $rc
    fi
done
exit "$failed"
