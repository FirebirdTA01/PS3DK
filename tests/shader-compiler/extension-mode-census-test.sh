#!/usr/bin/env bash
# t_397b859b: two-mode census.  Every tracked shader fixture is compiled with
# no extensions and again with --extension=bom, and the flag must move
# NOTHING except the constructs it names.
#
# This is the parent-free half of the extension evidence.  The other half - a
# named corpus against a build of the parent commit - is review evidence that
# needs a parent binary, which CI does not have; this one needs only the
# compiler under test, because the invariant is between two of its own modes.
#
# Rules, each of which a weaker census would miss:
#   - a row's exit status must be 0 or 1 in BOTH modes.  A timeout or a crash
#     is not a status, and two timeouts are not "the same result".
#   - exit 0 must leave a non-empty container; exit 1 must leave no file at
#     all.  Equal exit codes with a phantom or missing artifact are a defect.
#   - the sources the flag is ALLOWED to move are an exact list: each must
#     flip from refused to accepted, and nothing outside the list may change
#     status or bytes.  A listed flip that does not flip is as much a
#     failure as an unlisted one that does - the first is the extension not
#     reaching its construct, the second is the flag doing something it
#     does not claim.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-extension-mode-census.$$"
mkdir -p "$work/off" "$work/on"
trap 'rm -rf "$work"' EXIT

shaders_dir="tools/rsx-cg-compiler/tests/shaders"
FLAG='--extension=bom'

# The sources the flag may move, and only these: one leading mark on the
# main source, on an include, on a nested include, on both.  Paths relative
# to the repo root.  A BOM-only file is NOT here: with the mark consumed it
# is an empty file, which refuses in both modes for the empty file's reason.
expected_flips=(
    "$shaders_dir/extensions/bom_f.cg"
    "$shaders_dir/extensions/bom_include_f.cg"
    "$shaders_dir/extensions/bom_nested_include_f.cg"
    "$shaders_dir/extensions/bom_both_f.cg"
)

# Tracked fixtures only, so an untracked scratch file cannot make or break
# the census.  git is asked from the repo root; a linked worktree's .git
# file is followed by the git that created it.
list="$work/sources.txt"
git -C "$repo_root" ls-files -- "$shaders_dir" | grep -E '\.(cg|fcg|vcg)$' > "$list" \
    || fail "git ls-files produced no shader fixtures under $shaders_dir"
total="$(wc -l < "$list" | tr -d ' ')"
[[ "$total" -ge 100 ]] || fail "only $total tracked fixtures enumerated; the census list has collapsed"
for flip in "${expected_flips[@]}"; do
    grep -qxF "$flip" "$list" || fail "expected flip $flip is not a tracked fixture"
done

profile_for() {  # <path> -> profile from the naming convention
    case "$1" in
        *_v.cg|*.vcg) printf 'sce_vp_rsx' ;;
        *)            printf 'sce_fp_rsx' ;;
    esac
}

# compile <compiler> <mode off|on> <source> -> rc; container at
# $work/<mode>/<key>.bin (fresh), log beside it.
compile() {
    local cc="$1" mode="$2" src="$3" key out
    key="$(printf '%s' "$src" | tr '/.' '__')"
    out="$work/$mode/$key.bin"
    rm -f "$out"
    local -a flags=()
    [[ "$mode" == on ]] && flags=("$FLAG")
    local rc=0
    (
        cd "$repo_root"
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-25s}" "$cc" -p "$(profile_for "$src")" \
            -I "$repo_root/$shaders_dir" -I "$repo_root/$shaders_dir/extensions" \
            "${flags[@]}" --emit-container "$out" "$src"
    ) >"$work/$mode/$key.log" 2>&1 || rc=$?
    printf '%s' "$rc"
}

# census <compiler>: echoes one problem per line; empty output is a pass.
# Also writes the accepted/refused/flipped counts to $work/counts.
census() {
    local cc="$1" src off on key accepted=0 refused=0 flipped=0
    local -A flip_seen=()
    while IFS= read -r src; do
        key="$(printf '%s' "$src" | tr '/.' '__')"
        off="$(compile "$cc" off "$src")"
        on="$(compile "$cc" on "$src")"
        for mode_rc in "off:$off" "on:$on"; do
            local mode="${mode_rc%%:*}" rc="${mode_rc#*:}"
            case "$rc" in
                0)
                    [[ -s "$work/$mode/$key.bin" ]] || { printf '%s [%s]: exit 0 but no non-empty container\n' "$src" "$mode"; continue 2; }
                    ;;
                1)
                    [[ ! -e "$work/$mode/$key.bin" ]] || { printf '%s [%s]: exit 1 but left an artifact\n' "$src" "$mode"; continue 2; }
                    ;;
                124) printf '%s [%s]: timed out - not a status\n' "$src" "$mode"; continue 2 ;;
                *)   printf '%s [%s]: exit %s - a crash is not a status\n' "$src" "$mode" "$rc"; continue 2 ;;
            esac
        done
        local is_flip=0 flip
        for flip in "${expected_flips[@]}"; do
            [[ "$src" == "$flip" ]] && is_flip=1
        done
        if [[ "$is_flip" == 1 ]]; then
            if [[ "$off" == 1 && "$on" == 0 ]]; then
                flipped=$((flipped + 1)); flip_seen["$src"]=1
            else
                printf '%s: listed as a flip but went %s -> %s (expected 1 -> 0); the extension did not reach its construct\n' "$src" "$off" "$on"
            fi
            continue
        fi
        if [[ "$off" != "$on" ]]; then
            printf '%s: status moved %s -> %s under %s, and it is not on the flip list\n' "$src" "$off" "$on" "$FLAG"
            continue
        fi
        if [[ "$off" == 0 ]]; then
            accepted=$((accepted + 1))
            cmp -s "$work/off/$key.bin" "$work/on/$key.bin" \
                || printf '%s: bytes moved under %s on a source with no BOM\n' "$src" "$FLAG"
        else
            refused=$((refused + 1))
        fi
    done < "$list"
    for flip in "${expected_flips[@]}"; do
        [[ -n "${flip_seen[$flip]:-}" ]] || printf '%s: listed flip never judged\n' "$flip"
    done
    printf '%s %s %s\n' "$accepted" "$refused" "$flipped" > "$work/counts"
}

# ---------------------------------------------------------------------------
# SELF-CHECK FIRST, so a census function that judges nothing cannot report a
# green.  A stub that ignores the flag and accepts everything must be caught
# for the listed flips that did not flip; a stub that refuses everything
# must be caught the same way; a stub that accepts in one mode and refuses
# in the other must be caught for the unlisted flips.  Run on a short list
# so the self-check costs seconds.
short_list="$work/short.txt"
{ printf '%s\n' "${expected_flips[0]}"; grep -v '/extensions/' "$list" | head -n 3; } > "$short_list"
saved_list="$list"; list="$short_list"
make_stub() {  # <path> <script lines...>
    local path="$1"; shift
    { echo '#!/usr/bin/env bash'; printf '%s\n' "$@"; } > "$path"
    chmod +x "$path"
}
write_named_output='for a in "$@"; do case "$prev" in --emit-container) printf x > "$a";; esac; prev="$a"; done'
write_empty_output='for a in "$@"; do case "$prev" in --emit-container) : > "$a";; esac; prev="$a"; done'
make_stub "$work/stub-accept-all" "$write_named_output" 'exit 0'
problems="$(census "$work/stub-accept-all")"
grep -q 'listed as a flip but went 0 -> 0' <<<"$problems" || fail "self-check: an accept-everything stub was not caught on the listed flip:
$problems"
make_stub "$work/stub-refuse-all" 'exit 1'
problems="$(census "$work/stub-refuse-all")"
grep -q 'listed as a flip but went 1 -> 1' <<<"$problems" || fail "self-check: a refuse-everything stub was not caught on the listed flip:
$problems"
make_stub "$work/stub-flag-flips-all" 'case " $* " in *" --extension=bom "*) '"$write_named_output"'; exit 0;; esac' 'exit 1'
problems="$(census "$work/stub-flag-flips-all")"
grep -q 'not on the flip list' <<<"$problems" || fail "self-check: a stub that flips every source under the flag was not caught:
$problems"
make_stub "$work/stub-timeout" 'sleep 60'
problems="$(PS3TC_SHADER_TEST_TIMEOUT=2s census "$work/stub-timeout")"
grep -q 'timed out - not a status' <<<"$problems" || fail "self-check: a hanging stub was not caught:
$problems"
make_stub "$work/stub-phantom" "$write_empty_output" 'exit 1'
problems="$(census "$work/stub-phantom")"
grep -q 'exit 1 but left an artifact' <<<"$problems" || fail "self-check: a stub refusing with an artifact was not caught:
$problems"
list="$saved_list"
printf '  %-40s ok\n' "self-check: five stubs caught"

# ---------------------------------------------------------------------------
problems="$(census "$compiler")"
if [[ -n "$problems" ]]; then
    printf '%s\n' "$problems" >&2
    fail "the extension flag moved something it does not name, or failed to move something it does"
fi
read -r accepted refused flipped < "$work/counts"
printf '  %-40s %s sources: %s accepted identical in both modes, %s refused in both, %s flipped as listed\n' \
    "census" "$total" "$accepted" "$refused" "$flipped"
[[ "$flipped" -eq "${#expected_flips[@]}" ]] || fail "flipped $flipped, expected ${#expected_flips[@]}"
printf 'PASS: extension-mode-census-test\n'
