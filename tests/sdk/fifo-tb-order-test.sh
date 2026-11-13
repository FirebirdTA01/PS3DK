#!/usr/bin/env bash
# fifo-tb-order: the observed FIFO wrap's timestamp helper (ps3tc_fifo_tb in
# sdk/libgcm_cmd/src/ps3tc_fifo_wrap.c, PS3TC_FIFO_OBSERVE builds only) must
# compile, in both PPU data models, to exactly
#     hwsync ; mftb r3 ; isync ; blr
# (sync, the timebase read, isync: docs/design/ps3dk-prof-fifo.md, "Timestamp
# ordering").  This is a STATIC check of the emitted instruction sequence, not
# a hardware ordering test.
#
# Controls: the previous helper (a bare __builtin_ppc_get_timebase, which
# compiles to mftb ; blr) must FAIL the same check, and the default build's
# object (no PS3TC_FIFO_OBSERVE) must be byte-identical between the two
# helpers, compiled from the same source basename.
#
# Needs the PPU cross compiler: $PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc (or
# PPU_CC/PPU_OBJDUMP).  Without one it prints SKIP and exits 0.  The PPU
# system headers the file includes are replaced by minimal stubs, so no
# installed SDK is read; the helper does not depend on them.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
src="$repo_root/sdk/libgcm_cmd/src/ps3tc_fifo_wrap.c"
inc="$repo_root/sdk/libgcm_cmd/src"
cc="${PPU_CC:-${PS3DEV:-/nonexistent}/ppu/bin/powerpc64-ps3-elf-gcc}"
objdump="${PPU_OBJDUMP:-${cc%gcc}objdump}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ ! -x "$cc" || ! -x "$objdump" ]]; then
    echo "SKIP fifo-tb-order: no PPU cross compiler ($cc)"
    exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

st="$tmp/stub"
mkdir -p "$st/sys" "$st/rsx" "$st/cell/gcm"
cat > "$st/sys/timer.h" <<'EOF'
#pragma once
#include <stdint.h>
int sys_timer_usleep(uint64_t);
EOF
cat > "$st/sys/systime.h" <<'EOF'
#pragma once
#include <stdint.h>
uint64_t sysGetTimebaseFrequency(void);
EOF
cat > "$st/sys/lv2_types.h" <<'EOF'
#pragma once
#include <stdint.h>
#define lv2_fn_to_callback_ea(f) ((uintptr_t)(f))
EOF
: > "$st/ppu-asm.h"
cat > "$st/rsx/gcm_sys.h" <<'EOF'
#pragma once
#include <stdint.h>
typedef struct { volatile uint32_t put, get, ref; } gcmControlRegister;
typedef int32_t (*gcmContextCallback)(void *, uint32_t);
typedef struct _gcmCtxData { uint32_t *begin, *end, *current; gcmContextCallback callback; } gcmContextData;
int32_t gcmAddressToOffset(void *, uint32_t *);
gcmControlRegister *gcmGetControlRegister(void);
EOF
cat > "$st/cell/gcm/ps3tc_fifo_wrap.h" <<'EOF'
#pragma once
#include <rsx/gcm_sys.h>
int32_t ps3tc_fifo_wrap_callback(gcmContextData *, uint32_t);
void ps3tc_fifo_wrap_stats(uint64_t *, uint64_t *, uint64_t *);
void ps3tc_fifo_wrap_stats_reset(void);
void ps3tc_fifo_wrap_install(gcmContextData *);
EOF

# The previous helper, recreated from the current source (same basename, own directory).
mkdir -p "$tmp/old" "$tmp/new"
cp "$src" "$tmp/new/ps3tc_fifo_wrap.c"
python3 - "$src" "$tmp/old/ps3tc_fifo_wrap.c" <<'EOF'
import re, sys
text = open(sys.argv[1], encoding='utf-8').read()
start = text.index('static uint64_t ps3tc_fifo_tb(void *arg)\n{\n')
end = text.index('\n}\n', start) + 3
old = 'static uint64_t ps3tc_fifo_tb(void *arg)\n{\n    (void)arg;\n    return __builtin_ppc_get_timebase();\n}\n'
assert text.count('static uint64_t ps3tc_fifo_tb(void *arg)') == 1
open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write(text[:start] + old + text[end:])
EOF
cmp -s "$tmp/old/ps3tc_fifo_wrap.c" "$tmp/new/ps3tc_fifo_wrap.c" && fail "the control source equals the current source"

# The Makefile's CFLAGS, plus -ffunction-sections to isolate the helper's section.
flags=(-O2 -Wall -Wextra -Wno-unused-parameter -mcpu=cell -std=c11 -I"$st" -I"$inc" -ffunction-sections)

# seq OBJ: the helper's mnemonics, one per line
seq() {
    "$objdump" -d --section=.text.ps3tc_fifo_tb "$1" |
        awk -F'\t' '/^ *[0-9a-f]+:\t/ { split($3, m, " "); if (m[1] != "") print m[1] }'
}
want=$'hwsync\nmftb\nisync\nblr'

for abi in ilp32 lp64; do
    abiflag=()
    [[ $abi == lp64 ]] && abiflag=(-mlp64)
    for side in old new; do
        (cd "$tmp/$side" && "$cc" "${flags[@]}" "${abiflag[@]}" -DPS3TC_FIFO_OBSERVE=1 -c ps3tc_fifo_wrap.c -o "on-$abi.o")
        (cd "$tmp/$side" && "$cc" "${flags[@]}" "${abiflag[@]}" -c ps3tc_fifo_wrap.c -o "off-$abi.o")
    done
    got=$(seq "$tmp/new/on-$abi.o")
    [[ "$got" == "$want" ]] || fail "$abi observed helper is '$(echo $got)', expected '$(echo $want)'"
    old=$(seq "$tmp/old/on-$abi.o")
    [[ "$old" != "$want" ]] || fail "$abi control: the previous helper passed the ordered-sequence check"
    [[ "$old" == $'mftb\nblr' ]] || fail "$abi control: the previous helper is '$(echo $old)', expected 'mftb blr'"
    cmp "$tmp/old/off-$abi.o" "$tmp/new/off-$abi.o" || fail "$abi default (unobserved) objects differ"
    echo "$abi: observed helper = $(echo $got); previous helper = $(echo $old) (refused); default objects identical"
done
echo "fifo-tb-order-test: PASS"
