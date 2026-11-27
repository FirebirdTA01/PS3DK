#!/usr/bin/env bash
# Codegen guard for GCC patch 0084 (cell64lv2 bdnz/bdz hint).
#
# On Cell the bdnz/bdz counter-branch carries a taken/not-taken hint suffix
# drawn from the REG_BR_PROB on the branch insn.  The hint is one field of
# the instruction, not an extra insn, so it costs nothing at issue:
#
#   Short form (branch distance inside 16-bit range):
#       bdnz+ <label>    likely-taken
#       bdnz- <label>    likely-not-taken
#
#   Long form (distance outside the range, the pattern at rs6000.md:13751):
#       <bdz/bdnz>[+-] $+8      (negated counted branch; hint on THIS insn)
#       b <exit>
#   The counted branch is the *loop body start* jump; the "exit" is where a
#   taken counted-branch would fall through.  The hint on the negated bd is
#   inverted relative to the loop back-edge probability (need_long=true in
#   rs6000_bdnz_hint), so a likely-taken loop (hot back edge) shows a
#   likely-NOT-taken hint (bdz-) on the skip pair.
#
# A non-Cell powerpc* target emits the bare form (no suffix) because
# rs6000_bdnz_hint returns "" there.
#
# The test validates the ASSEMBLED output, not just mnemonic counts: each
# .s is run through the PPU assembler, so a malformed mnemonic, a wrong
# hint suffix, or a broken label reference fails instead of passing.  It
# covers (a) the hinted short form, (b) the forced long form with the
# inverted hint on the skip branch, and (c) a non-Cell control with no
# hint and a resolvable label.
#
# Negative controls: a compiler that exits non-zero, a missing output file,
# and a stale-output fallback must each FAIL the run.
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
as_="$ps3dev/ppu/bin/powerpc64-ps3-elf-as"
{ [ -x "$cxx" ] || [ -x "$cxx.exe" ]; } || {
    echo "ppu-bdnz-hint: FAIL: no PPU C++ compiler under $ps3dev"; exit 1
}
{ [ -x "$as_" ] || [ -x "$as_.exe" ]; } || {
    echo "ppu-bdnz-hint: FAIL: no PPU assembler under $ps3dev"; exit 1
}

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
row() { printf 'ppu-bdnz-hint: %s   %s\n' "$1" "$2"; }
failrow() { row FAIL "$*"; fail=1; }

# A short counter loop: body well inside the 16-bit range -> short bdnz/bdz.
cat > "$work/short.cpp" <<'EOF'
unsigned long sum_to (unsigned long n)
{
    unsigned long s = 0;
    for (unsigned long i = 1; i <= n; i++)
        s += i;
    return s;
}
EOF

# A counter loop whose body is padded far past 32 KiB, forcing the counted
# branch (pattern at rs6000.md:13751) to take the 8-byte long form:
#   <bdz/bdnz> [+-] $+8
#   b <exit>
# The hint on that negated bd is inverted relative to the hot back edge.
{
    printf 'unsigned long sum_long (unsigned long n, const volatile unsigned char *buf)\n{\n'
    printf '    unsigned long s = 0;\n'
    printf '    unsigned long a = buf[0];\n'
    printf '    for (unsigned long i = 1; i <= n; i++) {\n'
    printf '        s += i;\n'
    k=1
    while [ "$k" -le 5000 ]; do
        printf '        a ^= (unsigned long)buf[%s] * %s;\n' "$k" "$k"
        k=$((k + 1))
    done
    printf '    }\n    return s + a;\n}\n'
} > "$work/long.cpp"

# Count bdnz/bdz mnemonics that DO / DO NOT carry a + or - hint.
count_hinted() { grep -cE '^[[:space:]]*(bdnz|bdz)[+-][[:space:]]' "$1" 2>/dev/null; }
count_bare()   { grep -cE '^[[:space:]]*(bdnz|bdz)[[:space:]]'   "$1" 2>/dev/null; }
count_longform() { grep -cE '^[[:space:]]*(bdnz|bdz)[+-][[:space:]]+\$' "$1" 2>/dev/null; }

# compile_and_check <label> <output-file> <extra-flags...>
#  - checks the compiler exit status
#  - requires a non-empty, fresh output file (written in this call)
#  - assembles the output with the PPU as
#  - prints the hinted / bare / long counts on success
compile_and_check() {
    local label="$1" out="$2"; shift 2
    rm -f "$out"
    local rc
    if ! "$cxx" -std=c++17 "$@" -o "$out" 2> "$work/err"; then
        rc=$?
        [ -s "$work/err" ] && tail -5 "$work/err" | sed 's/^/    /'
        failrow "$label: compile failed (rc=$rc); expected success"
        return 1
    fi
    if [ ! -s "$out" ]; then
        failrow "$label: compiler exited 0 but produced no / empty .s (stale-output regression?)"
        return 1
    fi
    if ! "$as_" "$out" -o "$work/obj.o" --quiet 2> "$work/err"; then
        [ -s "$work/err" ] && tail -5 "$work/err" | sed 's/^/    /'
        failrow "$label: PPU assembler rejected the output (bad mnemonic / hint / label reference)"
        return 1
    fi
    return 0
}

# ============================================================================
# Row 1: Cell, short form -- require a hinted counted branch, no bare one.
# ============================================================================
for opt in -O1 -O2; do
    out="$work/short-$opt.s"
    if compile_and_check "Cell short $opt" "$out" "$opt" -S "$work/short.cpp"; then
        hinted=$(count_hinted "$out" || true); hinted=${hinted:-0}
        bare=$(count_bare   "$out" || true); bare=${bare:-0}
        if [ "$hinted" -lt 1 ]; then
            if [ "$bare" -gt 0 ]; then
                failrow "Cell short $opt: $bare BARE counted branch(es), 0 hinted (0084 not applied?)"
            else
                failrow "Cell short $opt: no bdnz/bdz counted branch at all (0084 not applied, or loop peeled at $opt)"
            fi
            continue
        fi
        if [ "$bare" -gt 0 ]; then
            failrow "Cell short $opt: $bare bare bdnz/bdz alongside $hinted hinted (0084 partially applied?)"
            continue
        fi
        row ok "Cell short $opt: $hinted hinted counted branch(es), assembles cleanly"
    fi
done

# ============================================================================
# Row 2: Cell, forced long form -- require the '<bdz/bdnz>[+-] $' pair.
# The hint here is inverted relative to the hot back edge.
# ============================================================================
out="$work/long.s"
if compile_and_check "Cell long" "$out" -O1 -S "$work/long.cpp"; then
    long=$(count_longform "$out" || true); long=${long:-0}
    if [ "$long" -lt 1 ]; then
        # The body may not have been large enough to force the 8-byte form,
        # or the loop was peeled.  Either way we have no long-form evidence.
        failrow "Cell long: no long-form counted branch ('<bdz/bdnz>[+-] \$' pair) in the .s -- body not long enough to force the 8-byte form, or loop peeled"
    else
        row ok "Cell long: $long long-form hinted pair(s) present, assembles cleanly"
    fi
fi

# ============================================================================
# Row 3: non-Cell control -- require bare bdnz (no hint) and a resolvable
# label.  Proves the suffix is a Cell-specific emission and does not leak.
# Some PS3DK GCC builds only ship the Cell target; -mcpu=power5 may be
# rejected.  If so, skip this row with a note (it is the control, not the
# guard for 0084 itself).
# ============================================================================
out="$work/pw5.s"
if "$cxx" -std=c++17 -O1 -mcpu=power5 -S "$work/short.cpp" -o "$out" 2> "$work/err"; then
    if [ -s "$out" ]; then
        bare=$(count_bare   "$out" || true); bare=${bare:-0}
        hinted=$(count_hinted "$out" || true); hinted=${hinted:-0}
        if [ "$hinted" -gt 0 ]; then
            failrow "non-cell: a + or - hint on bdnz/bdz under -mcpu=power5 (rs6000_bdnz_hint should return empty there; 0084 leaking?)"
            grep -E '^[[:space:]]*(bdnz|bdz)[+-]' "$out" | head -3 | sed 's/^/    /'
        elif [ "$bare" -lt 1 ]; then
            failrow "non-cell: no bare bdnz/bdz under -mcpu=power5 (this control cannot distinguish hint from no-hint; the target likely lowered the loop differently)"
        else
            if "$as_" "$out" -o "$work/obj-o.o" --quiet 2> "$work/err"; then
                row ok "non-cell -mcpu=power5: $bare bare counted branch(es), 0 hinted, assembles cleanly"
            else
                failrow "non-cell: PPU assembler rejected the power5 output (bad label reference?)"
                tail -5 "$work/err" | sed 's/^/    /'
            fi
        fi
    else
        failrow "non-cell: compiler exited 0 but no .s (stale-output regression?)"
    fi
else
    row "note" "non-cell -mcpu=power5: compiler rejected this row (target not built); skipping the control. The 0084 guard is the Cell short/long rows above."
fi

# ============================================================================
# Negative control: run this whole test against a mock PS3DEV whose compiler
# always fails and whose output file is pre-seeded with a stale hinted bdnz.
# The test MUST fail (old behaviour: a failed compile fell through to a WARN
# then PASSED on the stale loop.s, so the broken compiler was invisible).
#
# A PPU_BDNZ_HINT_INNER sentinel env var stops the recursive call from
# re-entering this block -- otherwise we would recurse forever.
# ============================================================================
if [ -z "${PPU_BDNZ_HINT_INNER:-}" ]; then
    fake="$(mktemp -d)"; trap 'rm -rf "$work" "$fake"' EXIT
    mkdir -p "$fake/ppu/bin"
    cat > "$fake/ppu/bin/powerpc64-ps3-elf-g++" <<'FAKE'
#!/usr/bin/env bash
echo "fake-cxx: refusing to compile" >&2
exit 7
FAKE
    chmod +x "$fake/ppu/bin/powerpc64-ps3-elf-g++"
    cat > "$fake/ppu/bin/powerpc64-ps3-elf-as" <<'FAKE'
#!/usr/bin/env bash
echo "fake-as: refusing" >&2
exit 9
FAKE
    chmod +x "$fake/ppu/bin/powerpc64-ps3-elf-as"

    PPU_BDNZ_HINT_INNER=1 bash "$0" --ps3dev "$fake" >/dev/null 2>&1
    inner_rc=$?
    [ "$inner_rc" -eq 0 ] && failrow "negative-control: test PASSED even though the compiler always fails (stale-output fallback not caught)" \
                        || row ok "negative-control: fake-broken compiler is correctly caught (inner test exited $inner_rc)"
fi

[ $fail -eq 0 ] && echo "ppu-bdnz-hint: PASS"
exit $fail
