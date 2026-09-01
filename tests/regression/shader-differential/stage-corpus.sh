#!/usr/bin/env bash
# stage-corpus.sh — corpus half of the shader-differential stager
# (increment 2).  Runs where the compiler lives (WSL/Linux); the
# PowerShell stager invokes it once and merges its output into
# dev_hdd0.
#
#   stage-corpus.sh <compiler> <corpus-root> <out-dir>
#
# For every fragment shader under <corpus-root> (*_f.cg, *.fcg;
# _work/ excluded), on BOTH lowering paths, compiles a fast and a
# --nofastmath container and sorts the outcome:
#
#   both refuse       -> ours-refused.txt row (name|path) — the rig's
#                        host-side `ours-refused` rows; no container
#                        exists so the guest never sees these.
#   verdict differs   -> flag-verdict-changes.txt row.  A flag that
#                        changes the VERDICT is a compiler finding on
#                        its own, reported loudly, never staged as a
#                        pixel pair (there is nothing to pair).
#   bytes identical   -> counted only.  Byte-identical implies
#                        pixel-identical; staging it would spend a
#                        guest draw to learn nothing.
#   bytes differ      -> the pair is staged and a manifest row is
#                        written: the pixel question "does this byte
#                        difference reach pixels?" is exactly what the
#                        rig exists to answer.
#
# Output layout under <out-dir>:
#   corpus/<name>@<path>_{fast,nofast}.fpo   staged containers
#   manifest-corpus.txt                      rows to append after the
#                                            standing controls
#   ours-refused.txt                         sidecar
#   flag-verdict-changes.txt                 findings (may be empty)
#
# Refuses a zero-compile run: an empty corpus or a broken compiler
# must not report as a clean sweep.

set -euo pipefail

fail() {
    printf 'stage-corpus: FAIL: %s\n' "$*" >&2
    exit 1
}

[[ $# -eq 3 ]] || fail "usage: stage-corpus.sh <compiler> <corpus-root> <out-dir>"
compiler="$1"; corpus="$2"; out="$3"
[[ -x "$compiler" ]] || fail "compiler not executable: $compiler"
[[ -d "$corpus" ]] || fail "corpus root not a directory: $corpus"
mkdir -p "$out/corpus"
: >"$out/manifest-corpus.txt"
: >"$out/ours-refused.txt"
: >"$out/flag-verdict-changes.txt"

compile_one() {
    # compile_one <src> <dst> <flags...>; rc 0 = container present
    local src="$1" dst="$2"
    shift 2
    local rc=0
    rm -f "$dst"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            "$@" -p sce_fp_rsx --emit-container "$dst" "$src"
    ) >/dev/null 2>&1 || rc=$?
    [[ "$rc" -eq 0 && -s "$dst" ]]
}

compiled=0; refused=0; identical=0; staged=0; flips=0
declare -A seen_names

while IFS= read -r src; do
    base="$(basename "$src")"
    base="${base%.cg}"; base="${base%.fcg}"
    rel="${src#"$corpus"/}"
    name="$base"
    if [[ -n "${seen_names[$name]:-}" ]]; then
        # disambiguate colliding basenames by a short path hash
        name="${base}_$(printf '%s' "$rel" | md5sum | cut -c1-6)"
    fi
    seen_names[$name]=1

    for path in default general; do
        pflag=()
        [[ "$path" == "general" ]] && pflag=(--general-lowering)
        fast="$out/corpus/${name}@${path}_fast.fpo"
        nofast="$out/corpus/${name}@${path}_nofast.fpo"
        okf=0; okn=0
        compile_one "$src" "$fast"   ${pflag[@]+"${pflag[@]}"} && okf=1
        compile_one "$src" "$nofast" ${pflag[@]+"${pflag[@]}"} --nofastmath && okn=1
        if [[ "$okf" -eq 0 && "$okn" -eq 0 ]]; then
            refused=$((refused+1))
            printf '%s|%s|%s\n' "$name" "$path" "$rel" >>"$out/ours-refused.txt"
            rm -f "$fast" "$nofast"
            continue
        fi
        if [[ "$okf" -ne "$okn" ]]; then
            flips=$((flips+1))
            printf '%s|%s|fast_rc0=%d|nofast_rc0=%d|%s\n' \
                "$name" "$path" "$okf" "$okn" "$rel" \
                >>"$out/flag-verdict-changes.txt"
            printf 'stage-corpus: VERDICT FLIP: %s [%s] fast=%d nofast=%d\n' \
                "$name" "$path" "$okf" "$okn" >&2
            rm -f "$fast" "$nofast"
            continue
        fi
        compiled=$((compiled+1))
        if cmp -s "$fast" "$nofast"; then
            identical=$((identical+1))
            rm -f "$fast" "$nofast"
        else
            staged=$((staged+1))
            printf 'B|probe|%s@%s|corpus/%s@%s_fast.fpo|corpus/%s@%s_nofast.fpo|0\n' \
                "$name" "$path" "$name" "$path" "$name" "$path" \
                >>"$out/manifest-corpus.txt"
        fi
    done
done < <(find "$corpus" -type f \( -name '*_f.cg' -o -name '*.fcg' \) \
           -not -path '*/_work/*' | sort)

printf 'SDIFF-STAGE|compiled=%d|refused=%d|identical=%d|staged=%d|verdict_flips=%d\n' \
    "$compiled" "$refused" "$identical" "$staged" "$flips"
[[ "$compiled" -gt 0 ]] || fail "zero shaders compiled — empty corpus or broken compiler"
