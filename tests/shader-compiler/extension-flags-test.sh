#!/usr/bin/env bash
# t_6f3fa9c3: the extension flag contract.
#
# An extension is a deliberate departure from the reference compiler, off by
# default so an unflagged compile stays reference-compatible in what it
# accepts and what it emits.  The flag surface is small and exact on purpose:
#
#   --extension=<name>    enable one named extension; repeatable; one name
#                         per flag; exact lowercase spelling
#   --list-extensions     print the supported names and exit 0
#   anything else         refused - no bare --extension, no empty name, no
#                         unknown name, no "all"
#
# Why the rules are this strict: the disabled-extension diagnostic tells the
# user WHAT TO TYPE.  That is only useful if the thing it prints is the thing
# the parser accepts, so this test does not grep for the spelling - it
# composes the flag from the --list-extensions output and FEEDS IT BACK.  A
# listed name the parser refuses, or an accepted spelling the list does not
# print, fails here.
#
# An enabled extension must change nothing for a source that does not use it:
# the plain fixture is compiled unflagged and flagged and the bytes compared.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-extension-flags-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

src="$work/plain_f.cg"
printf 'float4 main(float4 c : COLOR0) : COLOR { return c * 0.5; }\n' > "$src"

# run <compiler> <tag> <args...>: exit status in $rc, stdout/stderr split,
# container (if any) at $work/<tag>.fpo.  Every run names a FRESH output
# path so "no artifact" means this run wrote nothing, not that an earlier
# run's file was cleaned up.
run() {
    local cc="$1" tag="$2"; shift 2
    rc=0
    rm -f "$work/$tag.fpo"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$cc" "$@"
    ) >"$work/$tag.out" 2>"$work/$tag.err" || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$tag timed out"
    return 0
}

# ---------------------------------------------------------------------------
# 1. --list-extensions: exit 0, no input file needed, exact stdout.
#    Pinned exactly so a renamed or reworded extension is a visible change,
#    the way the preprocessor harness pins its stdout.
run "$compiler" list --list-extensions
[[ "$rc" -eq 0 ]] || { cat "$work/list.err" >&2; fail "--list-extensions exited $rc, expected 0"; }
expected_list="$work/expected-list.txt"
printf 'bom\taccept one leading UTF-8 byte order mark (EF BB BF) in the main source and in #include files; the reference refuses it\n' > "$expected_list"
# A Windows-hosted build writes stdout with CRLF; the table is pinned on
# its text, not its line ending.
tr -d '\r' < "$work/list.out" > "$work/list.out.lf" && mv "$work/list.out.lf" "$work/list.out"
cmp -s "$work/list.out" "$expected_list" || {
    printf -- '--- expected\n' >&2; cat "$expected_list" >&2
    printf -- '--- got\n' >&2; cat "$work/list.out" >&2
    fail "--list-extensions stdout is not the pinned table"
}
printf '  %-34s ok\n' "--list-extensions exact"

# Every listed name must be one the parser accepts, composed the way the
# diagnostics compose it: --extension= followed by the name.  Fed back, not
# grepped for.
names=()
while IFS=$'\t' read -r name _summary; do
    [[ -n "$name" ]] && names+=("$name")
done < "$work/list.out"
[[ "${#names[@]}" -ge 1 ]] || fail "--list-extensions listed no extension names"
for name in "${names[@]}"; do
    run "$compiler" "feed_$name" -p sce_fp_rsx "--extension=$name" --emit-container "$work/feed_$name.fpo" "$src"
    [[ "$rc" -eq 0 ]] || {
        cat "$work/feed_$name.err" >&2
        fail "--extension=$name is listed by --list-extensions but the parser refused it (exit $rc) - the list is not pasteable"
    }
    [[ -s "$work/feed_$name.fpo" ]] || fail "--extension=$name accepted the plain source but wrote no container"
done
printf '  %-34s ok\n' "listed names fed back: ${names[*]}"

# ---------------------------------------------------------------------------
# 2. --help documents both flags with the canonical spelling.
run "$compiler" help --help
[[ "$rc" -eq 0 ]] || fail "--help exited $rc"
grep -qF -- '--extension=<name>' "$work/help.err" || fail "--help does not document --extension=<name>"
grep -qF -- '--list-extensions' "$work/help.err" || fail "--help does not document --list-extensions"
printf '  %-34s ok\n' "--help names both flags"

# ---------------------------------------------------------------------------
# 3. An enabled extension changes NOTHING for a source that does not use it.
run "$compiler" plain -p sce_fp_rsx --emit-container "$work/plain.fpo" "$src"
[[ "$rc" -eq 0 ]] || { cat "$work/plain.err" >&2; fail "the plain fixture did not compile unflagged (exit $rc)"; }
[[ -s "$work/plain.fpo" ]] || fail "the plain fixture compiled but wrote no container"
cmp -s "$work/plain.fpo" "$work/feed_bom.fpo" \
    || fail "--extension=bom changed the bytes of a source with no BOM - an extension must be inert where its construct is absent"
run "$compiler" twice -p sce_fp_rsx --extension=bom --extension=bom --emit-container "$work/twice.fpo" "$src"
[[ "$rc" -eq 0 ]] || { cat "$work/twice.err" >&2; fail "repeating --extension=bom was refused (exit $rc); repeats are idempotent"; }
cmp -s "$work/plain.fpo" "$work/twice.fpo" || fail "repeating --extension=bom changed the bytes"
printf '  %-34s ok\n' "enabled extension inert on plain"

# ---------------------------------------------------------------------------
# 4. Refusals.  Each is checked through ONE function so the self-check below
#    can run the same function against a stub and watch it reject one.
#
#    The input file handed to the unknown-name runs DOES NOT EXIST.  If the
#    compiler refuses for the unknown name, the flag was validated at parse
#    time, before any file was read; if it refuses for the missing file, the
#    unknown name was tolerated and the refusal came from somewhere else.
missing="$work/does-not-exist.cg"
check_refusal() {  # <compiler> <tag> <expected stderr text> <args...>; echoes a problem, or nothing
    local cc="$1" tag="$2" want="$3"; shift 3
    run "$cc" "$tag" "$@"
    if [[ "$rc" -ne 1 ]]; then
        printf 'exited %s, expected exactly 1' "$rc"; return 0
    fi
    if [[ -e "$work/$tag.fpo" ]]; then
        printf 'refused but left an output artifact'; return 0
    fi
    if ! grep -qF -- "$want" "$work/$tag.err"; then
        printf 'stderr does not contain %s' "$want"; return 0
    fi
    if grep -qF 'cannot open' "$work/$tag.err"; then
        printf 'refused for the missing input file, not at flag parse time'; return 0
    fi
}
refuse() {  # <label> <expected stderr text> <args...>
    local label="$1" want="$2"; shift 2
    local tag="refuse_${label// /_}" problem
    problem="$(check_refusal "$compiler" "$tag" "$want" "$@" --emit-container "$work/$tag.fpo" "$missing")"
    [[ -z "$problem" ]] || { cat "$work/$tag.err" >&2; fail "$label: $problem"; }
    printf '  %-34s refused\n' "$label"
}
refuse "unknown name"            "unknown extension 'bogus'"  -p sce_fp_rsx --extension=bogus
refuse "unknown name lists bom"  "bom"                        -p sce_fp_rsx --extension=bogus
refuse "unknown after a good one" "unknown extension 'bogus'" -p sce_fp_rsx --extension=bom --extension=bogus
refuse "uppercase spelling"      "unknown extension 'BOM'"    -p sce_fp_rsx --extension=BOM
refuse "empty name"              "unknown extension ''"       -p sce_fp_rsx --extension=
refuse "comma list"              "unknown extension 'bom,x'"  -p sce_fp_rsx --extension=bom,x
refuse "bare --extension"        "--extension needs =<name>"  -p sce_fp_rsx --extension
refuse "all is not a name"       "unknown extension 'all'"    -p sce_fp_rsx --extension=all

# ---------------------------------------------------------------------------
# SELF-CHECK, one stub per assertion in check_refusal, each wrong in exactly
# one way so each assertion is seen to fail on its own.  Two of the stubs
# exit 1 correctly: a stub that only gets the exit status wrong would leave
# the artifact and body assertions unproven.
#   accept-all     exit 0, writes the container         -> exit-status assertion
#   right-body-artifact  exit 1, right body, leaves an EMPTY file -> artifact assertion
#   unrelated-body exit 1, no file, unrelated diagnostic -> body assertion
#   late-refusal   exit 1, refuses for the missing input -> parse-time assertion
make_stub() {  # <path> <script lines...>
    local path="$1"; shift
    { echo '#!/usr/bin/env bash'; printf '%s\n' "$@"; } > "$path"
    chmod +x "$path"
}
write_named_output='for a in "$@"; do case "$prev" in --emit-container) printf x > "$a";; esac; prev="$a"; done'
# The exit-1 artifact stub leaves an EMPTY file: the assertion is "no file at
# the output path", and a checker weakened to -s would miss exactly this.
write_empty_output='for a in "$@"; do case "$prev" in --emit-container) : > "$a";; esac; prev="$a"; done'
expect_stub_rejected() {  # <stub> <tag> <reason fragment>
    local stub="$1" tag="$2" reason="$3" problem
    problem="$(check_refusal "$stub" "$tag" "unknown extension 'bogus'" -p sce_fp_rsx --extension=bogus --emit-container "$work/$tag.fpo" "$missing")"
    [[ -n "$problem" ]] || fail "self-check: the $tag stub satisfied the unknown-name refusal - the assertion it was built to trip is not being made"
    case "$problem" in
        *"$reason"*) ;;
        *) fail "self-check: the $tag stub was rejected for the wrong reason: $problem" ;;
    esac
}
make_stub "$work/stub-accept-all" "$write_named_output" 'exit 0'
expect_stub_rejected "$work/stub-accept-all" stub_accept_all "expected exactly 1"
make_stub "$work/stub-artifact" "$write_empty_output" 'echo "rsx-cg-compiler: unknown extension '"'"'bogus'"'"'; supported: bom" >&2' 'exit 1'
expect_stub_rejected "$work/stub-artifact" stub_artifact "left an output artifact"
make_stub "$work/stub-unrelated" 'echo "stub.cg:1:1: error: unrelated stage failure" >&2' 'exit 1'
expect_stub_rejected "$work/stub-unrelated" stub_unrelated "stderr does not contain"
make_stub "$work/stub-late" 'echo "rsx-cg-compiler: cannot open does-not-exist.cg (unknown extension '"'"'bogus'"'"')" >&2' 'exit 1'
expect_stub_rejected "$work/stub-late" stub_late "not at flag parse time"
printf '  %-34s ok\n' "self-check: four stubs rejected"

printf 'PASS: extension-flags-test\n'
