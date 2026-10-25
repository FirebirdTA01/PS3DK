#!/usr/bin/env bash
# Exercise command failures and removed definitions in the real defaults test.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export REAL_GREP REAL_AWK MARKER
REAL_GREP=$(command -v grep)
REAL_AWK=$(command -v awk)
MARKER="$work/child-ran"
mkdir -p "$work/repo/tests/host" "$work/repo/patches/spu" "$work/bin"
cp "$root/tests/host/spu-gcc16-opt-defaults-test.sh" "$work/repo/tests/host/"
cp -R "$root/patches/spu/gcc-16.x" "$work/repo/patches/spu/"
test_script="$work/repo/tests/host/spu-gcc16-opt-defaults-test.sh"
cat > "$work/bin/grep" <<'SH'
#!/usr/bin/env bash
if [[ ${INJECT:-} == grep && "$*" == *OPT_flate_combine_instructions* ]]; then exit 2; fi
exec "$REAL_GREP" "$@"
SH
cat > "$work/bin/awk" <<'SH'
#!/usr/bin/env bash
if [[ ${INJECT:-} == awk && "$*" == *'n += '* ]]; then printf '0\n'; exit 71; fi
if [[ ${INJECT:-} == added && "$*" == *'f=gcc/'* ]]; then
    "$REAL_AWK" "$@" || exit $?
    exit 71
fi
exec "$REAL_AWK" "$@"
SH
cat > "$work/bad-version" <<'SH'
#!/usr/bin/env bash
if [[ $1 == -dumpversion ]]; then printf '16.1.0\n'; exit 71; fi
printf 'child invoked\n' >> "$MARKER"
exit 1
SH
chmod +x "$work/bin/grep" "$work/bin/awk" "$work/bad-version"
run() { # name injection expected-rc expected-text compiler
    local name=$1 injection=$2 expected=$3 needle=$4 compiler=$5 rc=0
    INJECT="$injection" SPU16_GCC="$compiler" PATH="$work/bin:$PATH" \
        bash "$test_script" > "$work/$name.log" 2>&1 || rc=$?
    if [[ $rc != "$expected" ]] || ! grep -Fq -- "$needle" "$work/$name.log"; then
        cat "$work/$name.log"
        echo "FAIL $name: rc $rc, want $expected; expected $needle"
        exit 1
    fi
    echo "ok $name"
}
run clean '' 0 'PASS: spu-gcc16-opt-defaults-test' ''
run grep-error grep 1 'grep failed (2)' ''
run awk-valid-output-error awk 1 'reading flag assignments failed' ''
run added-valid-output-error added 1 'reading spu-common.cc patch lines failed' ''
run version-valid-output-error '' 1 'version query failed' "$work/bad-version"
[[ ! -e "$MARKER" ]] || { echo 'FAIL compiler invoked after failed version query'; exit 1; }
for definition in TARGET_MODE_DEPENDENT_ADDRESS_P TARGET_OPTION_OPTIMIZATION_TABLE; do
    case $definition in
        TARGET_MODE_DEPENDENT_ADDRESS_P) file=gcc/config/spu/spu.cc; value=spu_mode_dependent_address_p ;;
        *) file=gcc/common/config/spu/spu-common.cc; value=spu_option_optimization_table ;;
    esac
    printf 'diff --git a/%s b/%s\n-#define %s %s\n' "$file" "$file" "$definition" "$value" \
        > "$work/repo/patches/spu/gcc-16.x/9999-test-remove-definition.patch"
    run "$definition-removed" '' 1 "$definition net define count 0" ''
done
echo 'PASS: spu-gcc16-opt-defaults-status-test (7 cases; failed version starts no compiler child)'
