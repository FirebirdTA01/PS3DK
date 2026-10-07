#!/usr/bin/env bash
# Reproducible __DATE__ / __TIME__: they must expand from the time the SHADER
# is compiled (or SOURCE_DATE_EPOCH), NOT from the compiler build's own date.
#
# WHAT WENT WRONG. initBuiltinMacros defined the two macros from the C
# __DATE__/__TIME__ of the compiler build: every compiler build differed
# byte-wise (releases were not reproducible), and a shader using __DATE__ got
# "the date the compiler was built" instead of "the date the shader was
# compiled".
#
# HOW THE VALUE IS OBSERVED.  A string macro cannot sit in a float4 return,
# and the `#if` evaluator has no string comparison, so the only observable
# channel is --dump-preprocess: emit the macro-expanded text and stop (C -E
# analogue).  --emit-container is used only in the control rows below, which
# prove a shader that does not expand the macros still compiles with an
# invalid SOURCE_DATE_EPOCH.
#
# LAYOUT.  Rows:
#   (a) positive: SOURCE_DATE_EPOCH=0 and =1234567890, and the date reached
#       through another macro's body, each asserted via
#       --dump-preprocess.  The day-1 form is verified negative (not zero-
#       padded, not the wrong year).  Leap-day rows (2000-02-29, 2100-03-01)
#       and 3001-01-01, past the range MSVC's gmtime_s accepts.
#   (b) boundary: SOURCE_DATE_EPOCH=253402300799 (end of year 9999) must
#       expand to "Dec 31 9999" / "23:59:59"; =253402300800 must refuse.
#   (c) invalid forms: 5 malformed values, each asserted via --dump-preprocess
#       on a fixture that USES the date macro - rc==1, stdout empty, stderr
#       names SOURCE_DATE_EPOCH.
#   (d) control rows: invalid SOURCE_DATE_EPOCH with a VALID shader that does
#       NOT expand the macros (plain, one that only tests defined(), and the
#       date inside an argument the callee drops, one and two levels deep),
#       --emit-container - rc==0, container written.
#       (The old eager implementation failed the whole compile here; lazy
#       must not.)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-date-time.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# Fixtures.  (a)/(b)/(c) go through --dump-preprocess and need a line that
# actually references the macro.  (d) uses a shader with no date reference so
# the lazy path never touches the env value.
cat > "$work/use_date.cg"  <<'EOF'
float4 main() : COLOR { return __DATE__; }
EOF
cat > "$work/use_time.cg" <<'EOF'
float4 main() : COLOR { return __TIME__; }
EOF
cat > "$work/plain.cg" <<'EOF'
float4 main() : COLOR { return 1.0; }
EOF
# The date reached only through another macro's body.
cat > "$work/wrapped.cg" <<'EOF'
#define BUILD_DATE __DATE__
float4 main() : COLOR { return BUILD_DATE; }
EOF
# defined() asks about the binding and must not read the variable.
cat > "$work/defined.cg" <<'EOF'
#if defined(__DATE__)
#endif
float4 main() : COLOR { return 1.0; }
EOF

# One --dump-preprocess run.  A non-zero exit fails naming the row; a bare
# $(...) under set -e would end the script with no message.
pp() {  # <epoch> <fixture>
    local out rc=0
    out="$(SOURCE_DATE_EPOCH="$1" "$compiler" -p sce_fp_rsx --dump-preprocess "$2" 2> "$work/pp.err")" || rc=$?
    [[ "$rc" -eq 0 ]] || fail "SOURCE_DATE_EPOCH=$1 $(basename "$2"): exit $rc ($(head -1 "$work/pp.err"))"
    printf '%s
' "$out"
}

# ---- (a) SOURCE_DATE_EPOCH=0 ------------------------------------------------
# C standard "Mmm dd yyyy" with day space-padded: "Jan  1 1970".
d="$(pp 0 "$work/use_date.cg")"
t="$(pp 0 "$work/use_time.cg")"
grep -qF 'return "Jan  1 1970"' <<<"$d" || fail "epoch 0 __DATE__ must expand to Jan  1 1970 (space-padded day)"
grep -qF  'return "Jan 01 1970"' <<<"$d" && fail "epoch 0 __DATE__ must not be zero-padded (Jan 01)"
grep -qF  'return "Jan  1 1969"' <<<"$d" && fail "epoch 0 __DATE__ must be year 1970, not 1969"
grep -qF 'return "00:00:00"' <<<"$t" || fail "epoch 0 __TIME__ must expand to 00:00:00"

# ---- (a) SOURCE_DATE_EPOCH=1234567890 --------------------------------------
# 1234567890s is 2009-02-13 23:31:30 UTC.  Different month/day/hour/min/second
# from epoch 0, so the row is non-vacuous.
d="$(pp 1234567890 "$work/use_date.cg")"
t="$(pp 1234567890 "$work/use_time.cg")"
grep -qF 'return "Feb 13 2009"' <<<"$d" || fail "epoch 1234567890 __DATE__ must expand to Feb 13 2009"
grep -qF 'return "23:31:30"'  <<<"$t" || fail "epoch 1234567890 __TIME__ must expand to 23:31:30"

# ---- (a) leap days: the calendar is computed in-tree, not by gmtime --------
# 951782400 = 2000-02-29 (century leap year), 4107542400 = 2100-03-01 (2100
# is not a leap year), 32535216000 = 3001-01-01 (past MSVC gmtime_s's range).
for row in '951782400 Feb 29 2000' '4107542400 Mar  1 2100' '32535216000 Jan  1 3001'; do
    e="${row%% *}"; want="${row#* }"
    d="$(pp $e "$work/use_date.cg")"
    grep -qF "return \"$want\"" <<<"$d" || fail "epoch $e __DATE__ must expand to $want"
done

# ---- (b) boundary: 253402300799 is the last valid SOURCE_DATE_EPOCH ----------
# 253402300799 = 9999-12-31 23:59:59 UTC.
d="$(pp 253402300799 "$work/use_date.cg")"
t="$(pp 253402300799 "$work/use_time.cg")"
grep -qF 'return "Dec 31 9999"' <<<"$d" || fail "epoch 253402300799 __DATE__ must expand to Dec 31 9999"
grep -qF 'return "23:59:59"'  <<<"$t" || fail "epoch 253402300799 __TIME__ must expand to 23:59:59"

# ---- (c) invalid: 5 forms + 253402300800 ------------------------------------
# Each refuses via --dump-preprocess: rc==1, stdout empty, stderr names
# SOURCE_DATE_EPOCH.
refuse_dp() {  # <value> <what>
    local bad="$1" what="$2"
    local rc=0 out err errf="$work/err.$bad.txt"
    out="$(SOURCE_DATE_EPOCH="$bad" "$compiler" -p sce_fp_rsx \
        --dump-preprocess "$work/use_date.cg" 2> "$errf")" || rc=$?
    [[ "$rc" -eq 1 ]]             || fail "$what: expected exit 1, got $rc"
    [[ -z "$out" ]]              || fail "$what: expected empty stdout, got $(printf '%s' "$out" | head -1)"
    grep -qF 'SOURCE_DATE_EPOCH' "$errf" || fail "$what: expected SOURCE_DATE_EPOCH in stderr, got $(head -1 "$errf")"
    printf '  refuse ok (%s): %s\n' "$bad" "$what"
}
refuse_dp "abc"          "non-integer"
refuse_dp "-5"           "negative"
refuse_dp "9abc"         "trailing token"
refuse_dp "1.5"          "float"
refuse_dp "0x10"         "hex (not a base-10 integer)"
refuse_dp "253402300800" "past the end of year 9999"

# ---- (a) the date reached through another macro ---------------------------
d="$(pp 0 "$work/wrapped.cg")"
grep -qF 'return "Jan  1 1970"' <<<"$d" || fail "a macro whose body is __DATE__ must expand to Jan  1 1970"
rc=0
SOURCE_DATE_EPOCH=abc "$compiler" -p sce_fp_rsx --dump-preprocess "$work/wrapped.cg" > /dev/null 2>&1 || rc=$?
[[ "$rc" -eq 1 ]] || fail "an invalid epoch must refuse the date reached through a macro: exit $rc"

# ---- (d) an argument the callee drops is never expanded ---------------------
# DROP never substitutes x, so the date inside its argument is only dry-scanned
# and must not read the variable; ONE substitutes it, so it must.
cat > "$work/drop.cg" <<'CG'
#define DROP(x) 1.0
float4 main() : COLOR { return DROP(__DATE__) + DROP(__TIME__); }
CG
cat > "$work/drop2.cg" <<'CG'
#define DROP(x) 1.0
float4 main() : COLOR { return DROP(DROP(__DATE__)); }
CG
cat > "$work/one.cg" <<'CG'
#define ONE(x) x
float4 main() : COLOR { return ONE(__DATE__); }
CG
for f in drop drop2; do
    rc=0
    out="$(SOURCE_DATE_EPOCH=abc "$compiler" -p sce_fp_rsx --dump-preprocess "$work/$f.cg" 2> "$work/err.$f.txt")" || rc=$?
    [[ "$rc" -eq 0 ]] || fail "$f.cg: a dropped argument read SOURCE_DATE_EPOCH: exit $rc ($(head -1 "$work/err.$f.txt"))"
    grep -qF 'return 1.0' <<<"$out" || fail "$f.cg: expected the DROP body 1.0 in the output"
done
d="$(pp 0 "$work/one.cg")"
grep -qF 'return "Jan  1 1970"' <<<"$d" || fail "a substituted argument __DATE__ must expand to Jan  1 1970"
rc=0
SOURCE_DATE_EPOCH=abc "$compiler" -p sce_fp_rsx --dump-preprocess "$work/one.cg" > /dev/null 2>&1 || rc=$?
[[ "$rc" -eq 1 ]] || fail "an invalid epoch must refuse a substituted argument __DATE__: exit $rc"

# ---- (d) defined(__DATE__) does not read the variable -------------------------
rm -f "$work/defined.fpo"
rc=0
SOURCE_DATE_EPOCH=abc "$compiler" -p sce_fp_rsx \
    --emit-container "$work/defined.fpo" "$work/defined.cg" 2> "$work/err.def.txt" || rc=$?
[[ "$rc" -eq 0 && -s "$work/defined.fpo" ]] \
    || fail "defined(__DATE__) read SOURCE_DATE_EPOCH: exit $rc ($(head -1 "$work/err.def.txt"))"

# ---- (d) control row: lazy evaluation, invalid epoch + plain shader ------
# The old eager implementation failed the WHOLE compile here (Preprocessor
# constructor read the env); the lazy one must not, because the shader does
# not reference __DATE__ or __TIME__.  The container path is exercised once.
rm -f "$work/plain.fpo"
out="$(SOURCE_DATE_EPOCH=abc "$compiler" -p sce_fp_rsx \
    --emit-container "$work/plain.fpo" "$work/plain.cg" 2> "$work/err.ctrl.txt")" || {
    rc=$?
    fail "control row: expected exit 0, got $rc ($(head -1 "$work/err.ctrl.txt"))"
}
[[ -s "$work/plain.fpo" ]] || fail "control row: expected a non-empty container, found none/empty"
printf '  control ok: an invalid epoch does not block a shader that does not use it\n'

echo "shader-date-time: PASS"
