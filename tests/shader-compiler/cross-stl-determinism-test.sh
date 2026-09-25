#!/usr/bin/env bash
# Two rsx-cg-compiler builds against DIFFERENT C++ standard libraries must
# emit byte-identical containers for every shader in the in-repo corpus.
#
# An if/else join once emitted its select instructions in unordered_set
# iteration order.  libstdc++ and the MSVC STL hash differently, so an
# MSVC-built compiler emitted a different program than a gcc-built one (3 of
# 421 containers, one of them aliased), and nothing in CI could see it: every
# CI build used libstdc++, and two builds sharing a hash function agree with
# each other whether or not the output depends on it.  This test compares
# builds that do NOT share one - CI pairs the gcc/libstdc++ build with a
# clang/libc++ build - so output that depends on container iteration order
# shows up as a byte difference.
#
# A shader both builds refuse (exit 1) is fine; one refused by only one build,
# or a container that differs, is a failure, and so is any outcome that is
# neither a container nor a refusal (see compile_all).  Adversarial self-tests
# cover a one-byte container difference, a one-sided refusal, and stub
# compilers that exit like a crash, a timeout, a timeout(1) error, or exit 0
# without writing a container; all must be rejected.
#
# Usage: cross-stl-determinism-test.sh <compiler-A> [compiler-B] [shader-dir]
# With one compiler (the form every script in this directory is run with by
# refusal-status-adversary-test.sh) B defaults to A: two runs of one build must
# agree, which still catches run-to-run nondeterminism.  CI passes both builds.
set -u

A="${1:?usage: $0 <compiler-A> [compiler-B] [shader-dir]}"
B="${2:-$A}"
root=$(cd "$(dirname "$0")/../.." && pwd)
corpus="${3:-$root/tools/rsx-cg-compiler/tests/shaders}"
status=0

fail() { echo "cross-stl-determinism: FAIL: $*" >&2; status=1; }
note() { echo "cross-stl-determinism: $*"; }

profile_for() {  # same naming convention as extension-mode-census-test.sh
    case "$1" in
        *_v.cg|*.vcg) printf 'sce_vp_rsx' ;;
        *)            printf 'sce_fp_rsx' ;;
    esac
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# compile_all <compiler> <outdir>: one outcome per shader.  Exactly two are
# ordinary: exit 0 with a non-empty container (kept as .bin), and exit 1, the
# compiler's refusal (a .refused marker).  Anything else fails the test in
# either build - exit 0 without a container, a timeout or timeout(1) error
# (124-127), a signal (>128), any other code - because two builds that fail the
# same way would otherwise compare equal.
compile_all() {
    local cc="$1" out="$2" src rel rc where
    mkdir -p "$out"
    while IFS= read -r src; do
        rel=${src#"$corpus"/}; rel=${rel//\//__}; where="$(basename "$cc") on ${src#"$corpus"/}"
        rm -f "$out/$rel.bin"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-25s}" "$cc" -p "$(profile_for "$src")" \
            --emit-container "$out/$rel.bin" "$src" >/dev/null 2>&1
        rc=$?
        if [ "$rc" -eq 0 ] && [ -s "$out/$rel.bin" ]; then
            continue
        elif [ "$rc" -eq 1 ]; then
            rm -f "$out/$rel.bin"; : > "$out/$rel.refused"
        elif [ "$rc" -eq 0 ]; then
            fail "$where: exit 0 but no container"
        else
            fail "$where: exit $rc (timeout, crash or infrastructure error - not a refusal)"
        fi
    done < <(find "$corpus" -type f \( -name '*.cg' -o -name '*.vcg' \) | LC_ALL=C sort)
}

# compare <dirA> <dirB> [quiet]: 0 when both directories hold the same
# outcome for every shader and identical bytes for every container.
compare() {
    local da="$1" db="$2" quiet="${3:-}" f base bad=0
    say_bad() { [ -n "$quiet" ] || echo "  $*" >&2; bad=1; }
    for f in "$da"/*; do
        base=$(basename "$f")
        if [ ! -e "$db/$base" ]; then
            say_bad "${base%.*}: outcome differs (${base##*.} in A only)"
        elif ! cmp -s "$f" "$db/$base"; then
            # .bin: the containers; .refused: the exit statuses
            say_bad "${base%.*}: ${base##*.} differs"
        fi
    done
    for f in "$db"/*; do
        [ -e "$da/$(basename "$f")" ] || say_bad "$(basename "$f"): present in B only"
    done
    return $bad
}

compile_all "$A" "$work/a"
compile_all "$B" "$work/b"
n=$(find "$corpus" -type f \( -name '*.cg' -o -name '*.vcg' \) | wc -l)
nbin=$(ls "$work/a" | grep -c '\.bin$')
[ "$n" -gt 0 ] || fail "no shaders found under $corpus"
[ "$nbin" -gt 0 ] || fail "compiler A produced no containers - the comparison below proves nothing"
if compare "$work/a" "$work/b"; then
    note "ok: $n shaders, $nbin containers byte-identical between the two builds"
else
    fail "the two builds disagree (see above) - output depends on the standard library"
fi

# Self-tests on copies of A's results.
cp -r "$work/a" "$work/m1"
first=$(ls "$work/m1" | grep '\.bin$' | head -n 1)
printf '\001' | dd of="$work/m1/$first" bs=1 seek=0 conv=notrunc 2>/dev/null
cmp -s "$work/a/$first" "$work/m1/$first" \
    && fail "self-test 1 did not change the container; the result below proves nothing"
if compare "$work/a" "$work/m1" quiet; then
    fail "self-test 1: a container differing in one byte was accepted"
else
    note "self-test 1 ok: a one-byte container difference is rejected"
fi

cp -r "$work/a" "$work/m2"
mv "$work/m2/$first" "$work/m2/${first%.bin}.refused"
: > "$work/m2/${first%.bin}.refused"
if compare "$work/a" "$work/m2" quiet; then
    fail "self-test 2: a shader compiled by only one build was accepted"
else
    note "self-test 2 ok: a one-sided refusal is rejected"
fi

# 3-6. Outcomes that are not a container or a refusal must fail the test even
#      when both builds produce them identically.  The stubs only EXIT with the
#      status a real crash or timeout would give: a real signal would make the
#      host write a core dump (WSL pipes cores past ulimit -c).
mkdir -p "$work/c3"
printf 'void main(out float4 c : COLOR) { c = float4(1,0,0,1); }\n' > "$work/c3/one_f.cg"
n=3
for stub in 'exit 139:a crash (signal exit)' 'exit 124:a timeout' 'exit 125:a timeout(1) error' 'exit 0:exit 0 with no container'; do
    printf '#!/usr/bin/env bash\n%s\n' "${stub%%:*}" > "$work/stub$n"
    chmod +x "$work/stub$n"
    if ( status=0; corpus="$work/c3"; compile_all "$work/stub$n" "$work/m$n"; exit $status ) 2>/dev/null; then
        fail "self-test $n: ${stub#*:} was accepted"
    else
        note "self-test $n ok: ${stub#*:} fails the test"
    fi
    n=$((n + 1))
done

exit $status
