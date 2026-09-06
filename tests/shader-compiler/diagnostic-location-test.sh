#!/usr/bin/env bash
# t_1366b9b9: a diagnostic must name the line the user wrote on.
#
# The driver composes the translation unit as
#   #line 1 "<builtin>"   + the embedded Cg standard-library header
#   #line 1 "<input>"     + the user's source
# and the lexer SKIPPED both markers without reading them, so every token
# carried its physical line in the composed stream.  Columns were right; the
# line was the source line plus everything ahead of it - 1 under
# --no-stdlib, 154 in the default composition.  A refusal that points 154
# lines past the mistake, into a header the user never wrote, is close to
# useless, and the count moves whenever the builtin header changes.
#
# Both compositions are checked here.  Not because one of them would pass
# on the unfixed compiler - exact line equality fails on both, at +1 and
# at +154 - but because they cover DIFFERENT offsets, which is what
# rejects a fix that compensates with a constant instead of reading the
# marker.  They do NOT reject a fix that honours only the LAST marker:
# the marker naming the user's source is last in both compositions, so
# such a shortcut passes here.  The included-header case below is what
# requires the earlier markers to be read too.
#
# The locations below are read out of the fixtures at run time rather than
# written down, so the test cannot rot when a fixture's comment header is
# edited: the assertion is that the compiler's number equals the source's,
# not that it equals a constant.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-diagnostic-location-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

# Split `<file>:<line>:<column>: error: ...` into file|line|column.  The
# file name is taken GREEDILY: a Windows path carries a drive colon
# (C:/src/x.cg:16:19: error: ...) and cutting at the first colon would call
# the file 'C' and the line '/src/x.cg'.  Only the last three colon-
# separated fields before ' error:' are structure.
parse_location() {
    printf '%s' "$1" | sed -nE 's/^(.*):([0-9]+):([0-9]+): error:.*$/\1|\2|\3/p'
}

# Self-test the parser before trusting it, on the shape this machine does
# not produce: a guard nobody has seen fail is not a guard.
synthetic='C:/src/repo/tools/x.cg:16:19: error: unknown type name '"'"'bogusType'"'"''
expected='C:/src/repo/tools/x.cg|16|19'
got_synthetic="$(parse_location "$synthetic")"
[[ "$got_synthetic" == "$expected" ]] || fail \
    "parse_location is broken before the test even runs: on a drive-lettered path it returned '$got_synthetic', expected '$expected'"

# Where 'bogusType' actually sits in each fixture: the line number and the
# 1-based column of its first character.
locate() {
    awk -v needle="bogusType" '
        index($0, needle) && $0 !~ /^[[:space:]]*\/\// {
            print NR, index($0, needle); exit
        }' "$1"
}

check() {
    local stem="$1" flags="$2" label="$3" want_column="${4:-yes}"
    local src="$shaders/$stem.cg"
    [[ -f "$src" ]] || fail "fixture missing: $src"

    local want
    want="$(locate "$src")"
    [[ -n "$want" ]] || fail "$stem: no uncommented 'bogusType' to locate"
    local want_line="${want%% *}" want_col="${want##* }"

    local log="$work/$stem.$label.log"
    local out="$work/$stem.$label.fpo"
    set +e
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_fp_rsx $flags --emit-container "$out" "$src"
    ) >"$log" 2>&1
    local status=$?
    set -e
    [[ $status -eq 1 ]] || fail "$stem ($label) exited $status, expected 1"

    # The first diagnostic is the one that names the mistake; the cascade
    # after it is a separate row (t_ec186f0c) and is not asserted here.
    local first
    first="$(grep -m1 -E ':[0-9]+:[0-9]+: error:' "$log" || true)"
    [[ -n "$first" ]] || {
        head -n 5 "$log" >&2
        fail "$stem ($label) emitted no located diagnostic"
    }

    local parsed
    parsed="$(parse_location "$first")"
    [[ -n "$parsed" ]] || fail "$stem ($label): could not parse a location out of: $first"
    local named="${parsed%%|*}"
    local rest="${parsed#*|}"
    local got_line="${rest%%|*}"
    local got_col="${rest##*|}"

    [[ "$got_line" == "$want_line" ]] || fail \
        "$stem ($label): diagnostic names line $got_line, the source has 'bogusType' on line $want_line (off by $((got_line - want_line))) - $first"
    # The local-declaration fixture's first diagnostic is still the
    # cascade one - it points at the identifier AFTER the unknown type -
    # and moving it is t_ec186f0c's row, not this one.  Asserting its
    # column here would pin the cascade in place.  The parameter fixture
    # has no cascade and its column is asserted.
    if [[ "$want_column" == yes ]]; then
        [[ "$got_col" == "$want_col" ]] || fail \
            "$stem ($label): diagnostic names column $got_col, expected $want_col - $first"
    fi
    [[ "$named" == "$src" ]] || fail \
        "$stem ($label): diagnostic names '$named', expected the path as given on the command line, '$src'"
    [[ -f "$out" ]] && fail "$stem ($label) refused but still wrote a container"
    printf '  %-28s %-12s %s:%s:%s\n' "$stem" "$label" "$(basename "$named")" "$got_line" "$got_col"
}

# The default composition carries the builtin header ahead of the source and
# is where the offset was 154; --no-stdlib carries one marker and is where it
# was 1.  A fix must hold for both.
check fp_unknown_type_param_f ""            default
check fp_unknown_type_param_f "--no-stdlib" no-stdlib
check fp_unknown_type_local_f ""            default   no
check fp_unknown_type_local_f "--no-stdlib" no-stdlib no

# The same marker pair surrounds every #include, so honouring it fixes a
# second wrong-file case that no fixture in the tree could show: a mistake
# in an included header was reported against the INCLUDING file, at a line
# in the composed unit.  Generated here because the tree has no fixture
# that includes a broken header, and it must not acquire one that a sweep
# would try to compile.
inc_dir="$work/inc"
mkdir -p "$inc_dir"
printf '// header line 1\n// header line 2\nbogusType inHeader;\n' > "$inc_dir/bad_header.h"
printf '#include \"bad_header.h\"\nvoid main(out float4 o : COLOR)\n{\n    o = 1.0;\n}\n' > "$work/uses_header.cg"
set +e
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
        -p sce_fp_rsx -I "$inc_dir" --emit-container "$work/uses_header.fpo" \
        "$work/uses_header.cg"
) >"$work/uses_header.log" 2>&1
inc_status=$?
set -e
[[ $inc_status -eq 1 ]] || fail "the included-header case exited $inc_status, expected 1"
inc_first="$(grep -m1 -E ':[0-9]+:[0-9]+: error:' "$work/uses_header.log" || true)"
[[ "$inc_first" == *"bad_header.h:3:1: error: unknown type name 'bogusType'"* ]] || fail \
    "a mistake on line 3 of an included header was reported as: ${inc_first:-<no located diagnostic>} - it must name bad_header.h line 3"
printf '  %-28s %-12s %s\n' "included header" "default" "bad_header.h:3:1"

# Reading the driver's markers is only half of it.  The preprocessor CONSUMES
# a directive line, and every line inside an inactive conditional, and emits
# nothing in its place; it also joins backslash-newline continuations before
# it starts counting; and the marker it emits on the way back out of an
# include named a composed-stream line.  Each of those loses source lines, so
# a shader with three #defines above the mistake reported it three lines
# early - and every one of these shapes is commoner in real shaders than the
# marker-only case the fixtures above cover.
#
# Generated rather than added as fixtures: they are one-line probes with no
# value as shaders, and the tree's sweeps compile every fixture they can see.
#
# The inactive_marker row is the one that says a marker must be OBEYED on
# the same terms as any other directive: a `#line` inside `#if 0` is
# suppressed, so it must rename nothing, and reading it anyway pointed the
# diagnostic for the live source after the #endif at a file that does not
# exist.  A suppressed directive changes nothing, exactly as a suppressed
# #define defines nothing.
#
# NOT COVERED HERE, deliberately: Preprocessor::setNoLineMarkers(true) must
# suppress the markers this file's fix generates, and it did not.  That
# option is API-only - main.cpp never calls it - so no test driving the
# compiler through its command line can reach it, and a proxy assertion
# here would only look like coverage.  It belongs to the preprocessor
# harness guard (t_e5fced4c), which links the preprocessor directly.
probe_dir="$work/probes"
mkdir -p "$probe_dir"
printf '// good header line 1\n#define HDRVAL 1.0\n' > "$probe_dir/good_header.h"

# name|the line the mistake is on|the file the diagnostic must name
# ('-' means the probe's own path)|body.  '@' stands in for a double
# quote, because the rows live inside double-quoted array elements.
probes=(
    "directives|6|-|#define A 1\n#define B 2\n#define C 3\nvoid main(float4 c : TEXCOORD0, out float4 o : COLOR)\n{\n    bogusType q;\n    o = c;\n}"
    "inactive|7|-|#if 0\nfloat unused1;\nfloat unused2;\n#endif\nvoid main(float4 c : TEXCOORD0, out float4 o : COLOR)\n{\n    bogusType q;\n    o = c;\n}"
    "after_include|5|-|#include @good_header.h@\n#define A 1\nvoid main(float4 c : TEXCOORD0, out float4 o : COLOR)\n{\n    bogusType q;\n    o = c;\n}"
    "spliced|6|-|#define LONG(a, b) \\\\\n    ((a) + (b))\n#define B 2\nvoid main(float4 c : TEXCOORD0, out float4 o : COLOR)\n{\n    bogusType q;\n    o = c;\n}"
    "user_marker|3|renamed_region.h|#line 1 @renamed_region.h@\nvoid main(float4 c : TEXCOORD0, out float4 o : COLOR)\n{\n    bogusType q;\n    o = c;\n}"
    "inactive_marker|4|-|#if 0\n#line 900 @hidden.h@\n#endif\nbogusType bad;"
)

for probe in "${probes[@]}"; do
    name="${probe%%|*}"
    rest="${probe#*|}"
    true_line="${rest%%|*}"
    rest="${rest#*|}"
    want_file="${rest%%|*}"
    body="${rest#*|}"
    body="${body//@/\"}"
    src="$probe_dir/$name.cg"
    printf '%b\n' "$body" > "$src"

    # A probe carrying its own #line marker renumbers itself, so its
    # physical line is not the line it claims and there is nothing to
    # check it against; every other probe is checked against its own text
    # so a table entry cannot silently drift from the file it describes.
    if [[ "$want_file" == "-" ]]; then
        actual_line="$(grep -n bogusType "$src" | head -1 | cut -d: -f1)"
        [[ "$actual_line" == "$true_line" ]] || fail \
            "probe $name is written wrong: bogusType is on line $actual_line, the table says $true_line"
    fi

    set +e
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_fp_rsx -I "$probe_dir" --emit-container "$probe_dir/$name.fpo" "$src"
    ) >"$probe_dir/$name.log" 2>&1
    probe_status=$?
    set -e
    [[ $probe_status -eq 1 ]] || fail "probe $name exited $probe_status, expected 1"

    probe_first="$(grep -m1 -E ':[0-9]+:[0-9]+: error:' "$probe_dir/$name.log" || true)"
    [[ -n "$probe_first" ]] || fail "probe $name emitted no located diagnostic"
    probe_parsed="$(parse_location "$probe_first")"
    probe_named="${probe_parsed%%|*}"
    probe_rest="${probe_parsed#*|}"
    probe_line="${probe_rest%%|*}"
    if [[ "$want_file" == "-" ]]; then
        [[ "$probe_named" == "$src" ]] || fail \
            "probe $name: diagnostic names '$probe_named', expected the probe path '$src'"
    else
        [[ "$probe_named" == "$want_file" ]] || fail \
            "probe $name: diagnostic names '$probe_named', expected '$want_file' - a #line marker the USER wrote must rename the region like the driver's own"
    fi
    [[ "$probe_line" == "$true_line" ]] || fail \
        "probe $name: the mistake is on line $true_line and the diagnostic names line $probe_line (off by $((probe_line - true_line))) - $probe_first"
    printf '  %-28s %-12s line %s\n' "$name" "probe" "$probe_line"
done

# CONTROL: reading the markers must not disturb a program that compiles.
control_out="$work/control.fpo"
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
        -p sce_fp_rsx --emit-container "$control_out" \
        "$shaders/fp_known_sampler_control_f.cg"
) >"$work/control.log" 2>&1 || {
    tail -n 10 "$work/control.log" >&2
    fail "the control shader stopped compiling"
}
[[ -s "$control_out" ]] || fail "the control shader wrote no container"

printf 'diagnostic-location-test: ok (both compositions name the source line and column, and the control still compiles)\n'
printf 'PASS: diagnostic-location-test\n'
