#!/usr/bin/env bash
# Codegen guard for GCC patch 0085: on the Cell PPU the AIX traceback
# table (tbtab) is dropped by default, but an explicit -mtraceback
# (full or part) must still override.
#
# The tbtab is emitted by rs6000-logue.cc at the end of each function
# in .text as:
#     <fn>LT<fn>:
#     .long 0
#     .byte 0,<lang>,...
# after `blr`, before the next function/section.
#
# The gate is:
#     (DEFAULT_ABI == ABI_AIX || DEFAULT_ABI == ABI_ELFv2)
#      && rs6000_traceback != traceback_none
#      && ! cfun->is_thunk
#
# So for a single non-thunk function, in both data models:
#     default                 -> 0 tbtabs
#     -mtraceback=full        -> >= 1 tbtab
#     -mtraceback=part        -> >= 1 tbtab
#     -mtraceback=no          -> 0 tbtabs
#
# The test writes a FRESH output file per row (not a reused loop.s),
# checks the compiler exit status, and treats a compiler failure as a
# hard fail.  A negative control (a broken compiler) must make the run
# FAIL, not pass.
#
# Skips without PS3DEV.
#
# Usage: tests/sdk/ppu-traceback-off-test.sh [--ps3dev DIR]
set -u

ps3dev="${PS3DEV:-}"
[ "${1:-}" = --ps3dev ] && ps3dev="${2:-}"
if [ -z "$ps3dev" ]; then
    echo "ppu-traceback-off: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
{ [ -x "$cc" ] || [ -x "$cc.exe" ]; } || { echo "ppu-traceback-off: FAIL: no PPU C compiler under $ps3dev"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/t.c" <<'EOF'
int f (int a, int b) { return a + b; }
EOF

fail=0
row   () { printf 'ppu-traceback-off: %s   %s\n' "$1" "$2"; }
failrow() { row FAIL "$*"; fail=1; }

# Count tbtab occurrences: a tbtab is a ".long 0" followed by ".byte 0,".
count_tbtab() {
  awk '
    /^[[:space:]]*\.long[[:space:]]+0[[:space:]]*$/ { pending = 1; next }
    pending && /^[[:space:]]*\.byte[[:space:]]+0,/{ n++; pending = 0 }
    { pending = 0 }
    END { print n + 0 }
  ' "$1"
}

# run_row <label> <expected> <abi-flag-or-empty> <extra-flags...>
#   expected: 'none' (must be 0) or 'present' (must be >= 1)
run_row() {
    local label="$1" expect="$2" abi="$3"; shift 3
    local out="$work/${label// /_}.s"
    rm -f "$out"
    if ! "$cc" ${abi} "$@" -S "$work/t.c" -o "$out" 2> "$work/err"; then
        [ -s "$work/err" ] && tail -3 "$work/err" | sed 's/^/    /'
        failrow "$label: compile failed (rc=$?); expected success"
        return
    fi
    if [ ! -s "$out" ]; then
        failrow "$label: compiler exited 0 but produced no / empty .s (stale-output regression?)"
        return
    fi
    local n; n=$(count_tbtab "$out")
    case "$expect" in
        none)
            if [ "$n" -eq 0 ]; then row ok "$label: no tbtab in asm (correct)"; else
                failrow "$label: $n tbtab(s) present (0085 not applied? / -mtraceback=no clobbered?)"; fi ;;
        present)
            if [ "$n" -ge 1 ]; then row ok "$label: $n tbtab(s) present (override preserved)"; else
                failrow "$label: 0 tbtab(s) (override clobbered by 0085?)"; fi ;;
    esac
}

for abi in "" -mlp64; do
    model="ilp32"; [ -n "$abi" ] && model="lp64"

    # 0085 default: 0 tbtab ---------------------------------------------
    run_row "$model default"   none "$abi"
    # -mtraceback=full: >= 1 tbtab (user override preserved) ------------
    run_row "$model full"      present "$abi" -mtraceback=full
    # -mtraceback=part: >= 1 tbtab (documented override) ---------------
    run_row "$model part"      present "$abi" -mtraceback=part
    # -mtraceback=no: 0 tbtab ------------------------------------------
    run_row "$model no"        none "$abi" -mtraceback=no
done

# ============================================================================
# Negative control: this whole test against a mock PS3DEV whose compiler
# always fails.  It MUST fail (a broken-compiler run must not be able to
# pass on a stale or missing .s).  A sentinel env var stops the recursion.
# ============================================================================
if [ -z "${PPU_TRACEBACK_INNER:-}" ]; then
    fake="$(mktemp -d)"; trap 'rm -rf "$work" "$fake"' EXIT
    mkdir -p "$fake/ppu/bin"
    cat > "$fake/ppu/bin/powerpc64-ps3-elf-gcc" <<'FAKE'
#!/usr/bin/env bash
echo "fake-cc: refusing to compile" >&2
exit 7
FAKE
    chmod +x "$fake/ppu/bin/powerpc64-ps3-elf-gcc"

    PPU_TRACEBACK_INNER=1 bash "$0" --ps3dev "$fake" >/dev/null 2>&1
    inner_rc=$?
    [ "$inner_rc" -eq 0 ] && failrow "negative-control: test PASSED even though the compiler always fails (stale-output fallback not caught)" \
                        || row ok "negative-control: fake-broken compiler is correctly caught (inner test exited $inner_rc)"
fi

[ $fail -eq 0 ] && echo "ppu-traceback-off: PASS"
exit $fail
