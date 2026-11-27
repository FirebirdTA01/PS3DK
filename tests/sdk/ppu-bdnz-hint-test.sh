#!/usr/bin/env bash
# Codegen guard for GCC patch 0084:
# on the Cell PPU, the counter-branch family (bdnz/bdz and the long
# bdnz->b pair) now carries a + (likely-taken) or - (likely-not-taken)
# hint drawn from the REG_BR_PROB note on the branch insn.
#
# Before 0084 the emitted asm used the bare form:
#   bdnz <label>
# After 0084, on Cell with -O1 or better (so that a branch-probability
# note exists):
#   bdnz+ <label>    (loop back edge, likely taken)
#   bdnz- <label>    (less common: exit edge)
#
# The hint is a branch-hint field in the instruction encoding (not an
# extra NOP).  The assembler for powerpc64-ps3-elf accepts it.
#
# Non-Cell targets: rs6000_bdnz_hint returns "" when rs6000_cpu is
# not PROCESSOR_CELL, so no hint is emitted and the asm is unchanged.
#
# The test compiles a small counter loop and looks for a + or -
# suffix directly on the bdnz/bdz mnemonic in the .s output.
#
# Skips without PS3DEV.
#
# Usage: tests/sdk/ppu-bdnz-hint-test.sh [--ps3dev DIR]
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "ppu-bdnz-hint: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
{ [ -x "$cxx" ] || [ -x "$cxx.exe" ]; } || {
    echo "ppu-bdnz-hint: FAIL: no PPU C++ compiler under $ps3dev"
    exit 1
}

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

cat > "$work/loop.cpp" <<'EOF'
int sum_range (int n)
{
    int s = 0;
    for (int i = 1; i <= n; i++)
        s += i;
    return s;
}

int count_set_bits (unsigned long x)
{
    int c = 0;
    while (x)
    {
        x &= x - 1;
        c++;
    }
    return c;
}
EOF

fail=0

for opt in -O1 -O2; do
    err=$("$cxx" -std=c++17 $opt -S "$work/loop.cpp" -o "$work/loop.s" 2>&1)
    if [ -s "$err" ] && ! [ -f "$work/loop.s" ]; then
        echo "ppu-bdnz-hint: FAIL $opt: compiler error:"
        printf '%s\n' "$err" | head -5 | sed 's/^/    /'
        fail=1
        continue
    fi

    hint_count=$(grep -cE '^[[:space:]]*(bdnz|bdz)[+-]' "$work/loop.s" || true)
    total_count=$(grep -cE '^[[:space:]]*(bdnz|bdz)' "$work/loop.s" || true)
    bare_count=$(( total_count - hint_count ))
    [ "$bare_count" -lt 0 ] && bare_count=0

    if [ "$hint_count" -gt 0 ]; then
        echo "ppu-bdnz-hint: ok  $opt: $hint_count hinted bdnz/bdz insn(s) found"
    elif [ "$bare_count" -gt 0 ]; then
        echo "ppu-bdnz-hint: FAIL $opt: $bare_count bare bdnz/bdz insn(s), 0 hinted (0084 not applied?)"
        fail=1
    else
        echo "ppu-bdnz-hint: WARN $opt: no bdnz/bdz insn at all in output (GCC did not use the counted-branch form)"
        # Not a hard fail; the loop might have been peeled or unrolled away at -O2
    fi
done

[ $fail -eq 0 ] && echo "ppu-bdnz-hint: PASS"
exit $fail
