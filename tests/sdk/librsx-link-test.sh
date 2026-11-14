#!/usr/bin/env bash
# Tests that librsx maintains symbol isolation and legacy linkability.
#
# Commands in librsx.a must retain standalone PSL1GHT bodies and not depend on
# native ps3tc_gcm_state_* emitters (which belong to libgcm_cmd).
#
# Rows:
#   nm: verify that commands.o (and installed librsx.a) defines its own symbols
#       and has NO undefined ps3tc_gcm_state_* references, in ILP32 and LP64.
#   link: verify that a minimal PPU consumer calling rsxSetReport and
#         rsxSetTransferData links cleanly against librsx in both ABIs (rc 0).
#
# Skips without PS3DEV (CI host-tools-cxx has no PPU compiler).
# usage: librsx-link-test.sh [--ps3dev DIR] [--lib DIR] [--include DIR]...
set -u

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
ps3dev="${PS3DEV:-}"; lib=""; incs=()
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        --lib) lib="$2"; shift 2 ;;
        --include) incs+=("-I$2"); shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR] [--lib DIR] [--include DIR]..." >&2; exit 2 ;;
    esac
done

if [ -z "$ps3dev" ]; then
    echo "librsx-link: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi

[ ${#incs[@]} -gt 0 ] || incs=(
    "-I$root/sdk/include"
    "-I$root/sdk/include/rsx"
    "-I$root/sdk/libgcm_cmd/include"
)

if [ -z "$lib" ]; then
    if [ -n "${PS3DK:-}" ]; then
        lib="$PS3DK/ppu/lib"
    else
        lib="$ps3dev/ps3dk/ppu/lib"
    fi
fi

export PS3DEV="$ps3dev"
[ -n "${PS3DK:-}" ] || export PS3DK="$ps3dev/ps3dk"

bin="$ps3dev/ppu/bin/powerpc64-ps3-elf"
status=0

fail() { echo "librsx-link: FAIL: $*"; status=1; }
ok() { echo "librsx-link: ok   $*"; }

[ -x "$bin-gcc" ] || [ -x "$bin-gcc.exe" ] || { echo "librsx-link: FAIL: no PPU compiler under $ps3dev"; exit 1; }
[ -x "$bin-nm" ] || [ -x "$bin-nm.exe" ] || { echo "librsx-link: FAIL: no PPU nm under $ps3dev"; exit 1; }

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT

# Minimal PPU consumer exercising rsxSetReport and rsxSetTransferData
cat << 'EOF' > "$work/consumer.c"
#include <rsx/commands.h>

int main(void)
{
    gcmContextData *context = (gcmContextData *)0;
    rsxSetReport(context, 1, 2);
    rsxSetTransferData(context, 0, 0x1000, 64, 0x2000, 64, 64, 16);
    return 0;
}
EOF

for abi in "" -mlp64; do
    label="ilp32"
    lib_dir="$lib"
    if [ "$abi" = "-mlp64" ]; then
        label="lp64"
        lib_dir="$lib/lp64"
    fi

    # 1. Compile sdk/librsx/src/commands.c from working tree
    cmd_obj="$work/commands_${label}.o"
    if "$bin-gcc" $abi -O2 -Wall -mcpu=cell -mregnames -Wa,-mcell -std=gnu11 \
            -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections \
            -Wno-unused-parameter -Wno-implicit-fallthrough \
            -Wno-implicit-function-declaration \
            "${incs[@]}" -I"$ps3dev/ps3dk/ppu/include" -I"$ps3dev/ps3dk/ppu/include/rsx" \
            -DLIBRSX_INTERNAL -c "$root/sdk/librsx/src/commands.c" -o "$cmd_obj" > "$work/compile_${label}.log" 2>&1; then
        ok "compile commands.c ($label)"
    else
        fail "compile commands.c ($label): $(cat "$work/compile_${label}.log")"
        continue
    fi

    # 2. nm check: ensure commands.o has no undefined ps3tc_gcm_state_* symbols
    undef=$("$bin-nm" -u "$cmd_obj" 2>/dev/null | grep 'ps3tc_gcm_state_' || true)
    if [ -n "$undef" ]; then
        fail "commands.o ($label) has undefined native state symbols: $undef"
    else
        ok "commands.o ($label) has no undefined ps3tc_gcm_state_* symbols"
    fi

    # If installed archive exists, verify archive's symbols too
    archive="$lib_dir/librsx.a"
    if [ -f "$archive" ]; then
        undef_ar=$("$bin-nm" -u "$archive" 2>/dev/null | grep 'ps3tc_gcm_state_' || true)
        if [ -n "$undef_ar" ]; then
            fail "archive $archive has undefined native state symbols: $undef_ar"
        else
            ok "archive $archive has no undefined ps3tc_gcm_state_* symbols"
        fi
    fi

    # 3. Link consumer with commands.o and -lrsx
    if [ -f "$archive" ]; then
        if "$bin-gcc" $abi -O2 "${incs[@]}" -I"$ps3dev/ps3dk/ppu/include" \
                "$work/consumer.c" "$cmd_obj" -L"$lib_dir" -lrsx -o "$work/consumer_tree_${label}.elf" > "$work/link_tree_${label}.log" 2>&1; then
            ok "link consumer ($label) with tree commands.o + -lrsx"
        else
            fail "link consumer ($label) with tree commands.o + -lrsx: $(cat "$work/link_tree_${label}.log")"
        fi

        # Also pure -lrsx link
        if "$bin-gcc" $abi -O2 "${incs[@]}" -I"$ps3dev/ps3dk/ppu/include" \
                "$work/consumer.c" -L"$lib_dir" -lrsx -o "$work/consumer_lib_${label}.elf" > "$work/link_lib_${label}.log" 2>&1; then
            ok "link consumer ($label) with installed -lrsx"
        else
            fail "link consumer ($label) with installed -lrsx: $(cat "$work/link_lib_${label}.log")"
        fi
    fi
done

if [ $status -eq 0 ]; then
    echo "librsx-link: PASS"
fi
exit $status
