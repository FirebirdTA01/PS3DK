#!/usr/bin/env bash
# scripts/ppu-thread-model.sh and its use by build-ppu-toolchain.sh's GCC step: a build tree, an installed prefix
# and a --host build's native source stage are reused only when all carry the wanted thread model, and both host
# configurations pass --enable-threads.
#
#  1. defaults: GCC 16 posix, GCC 12 single; the configure argument;
#  2. reuse decisions on stamped temp trees: fresh, matching, tree missing/mismatching stamp (rebuild), installed
#     tree with the prefix stamp missing/mismatching (reinstall: the early return must not skip it), native source
#     missing/mismatching (native, fatal);
#  3. ppu_configured_threads reads the configure argv recorded in config.status (and "none" without it);
#  4. build-ppu-toolchain.sh: the threads argument is in the conf_args shared by both host branches (not inside one),
#     no other --enable-threads is passed, and the model check runs before the early "already installed" return.
#
# usage: ppu-thread-model-test.sh [--build-script <path>]   (default: the repo's; another one is a control)
#   Exit 0 pass, 1 fail, 2 usage.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
lib="$root/scripts/ppu-thread-model.sh"
build="$root/scripts/build-ppu-toolchain.sh"
while [ $# -gt 0 ]; do
    case $1 in
        --build-script) build=$2; shift 2 ;;
        *) echo "usage: $0 [--build-script <path>]"; exit 2 ;;
    esac
done
# shellcheck source=../../scripts/ppu-thread-model.sh
source "$lib" || { echo "ppu-thread-model: cannot source $lib"; exit 1; }

status=0
ok()   { echo "ppu-thread-model: ok   $*"; }
fail() { echo "ppu-thread-model: FAIL $*"; status=1; }
expect() { [ "$2" = "$3" ] && ok "$1: $2" || fail "$1: got '$2', want '$3'"; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# 1
expect "default for GCC 16" "$(ppu_threads_default 16)" posix
expect "default for GCC 12" "$(ppu_threads_default 12)" single
expect "configure argument" "$(ppu_gcc_threads_arg posix)" "--enable-threads=posix"

# 2: case <label> <want> <expected> <obj stamp|-|none> <installed 0/1> <prefix stamp|-> [<native stamp|->]
#    "-" = no stamp file; "none" = no build tree at all.
case_() {
    local label=$1 want=$2 expected=$3 objs=$4 inst=$5 pres=$6 nats=${7:-}
    local d="$work/$label" native=""
    rm -rf "$d"; mkdir -p "$d/prefix"
    if [ "$objs" != none ]; then
        mkdir -p "$d/obj"
        [ "$objs" != - ] && echo "$objs" > "$d/obj/.threads-model"
        [ "$inst" = 1 ] && touch "$d/obj/.installed"
    fi
    [ "$pres" != - ] && echo "$pres" > "$d/prefix/.ps3dk-thread-model"
    if [ -n "$nats" ]; then
        native="$d/native"; mkdir -p "$native"
        [ "$nats" != - ] && echo "$nats" > "$native/.ps3dk-thread-model"
    fi
    expect "$label" "$(ppu_stage_model_decision "$want" "$d/obj" "$d/prefix" "$native")" "$expected"
}
case_ fresh                  posix ok        none   0 -
case_ all-match              posix ok        posix  1 posix
case_ tree-other-model       posix rebuild   single 1 posix
case_ tree-unstamped         posix rebuild   -      1 posix
case_ prefix-unstamped       posix reinstall posix  1 -
case_ prefix-other-model     posix reinstall posix  1 single
case_ prefix-other-uninstalled posix ok      posix  0 single
case_ cross-native-unstamped posix native    posix  1 posix -
case_ cross-native-other     posix native    posix  1 posix single
case_ cross-all-match        posix ok        posix  1 posix posix
case_ single-all-match       single ok       single 1 single

# 3
mkdir -p "$work/cs"
printf '%s\n' "ac_cs_config=\"'--prefix=/x' '--target=powerpc64-ps3-elf' '--enable-threads=posix' '--disable-nls'\"" > "$work/cs/config.status"
expect "configured threads from config.status" "$(ppu_configured_threads "$work/cs")" posix
: > "$work/cs/config.status"
expect "configured threads without the argument" "$(ppu_configured_threads "$work/cs")" none
expect "configured threads without config.status" "$(ppu_configured_threads "$work/none")" none

# 4: the build script's use.
[ -f "$build" ] || { fail "missing $build"; echo "FAIL: ppu-thread-model-test"; exit 1; }
grep -q 'source "$script_dir/ppu-thread-model.sh"' "$build" && ok "build script sources the library" \
    || fail "build script does not source scripts/ppu-thread-model.sh"
# The threads argument sits in the shared conf_args=( ... ) list, which closes before the host-branch split.
shared=$(awk '/local conf_args=\(/ {f=1} f {print} f && /^    \)$/ {exit}' "$build")
grep -q 'ppu_gcc_threads_arg "\$threads_mode"' <<< "$shared" && ok "threads argument in the conf_args shared by both host branches" \
    || fail "the threads argument is not in the shared conf_args list"
n=$(grep -v '^[[:space:]]*#' "$build" | grep -c -- '--enable-threads')
[ "$n" -eq 0 ] && ok "no branch-specific --enable-threads literal" || fail "$n --enable-threads literal(s) left in the build script"
skip_line=$(grep -n 'GCC already built and installed (skipping)' "$build" | head -n 1 | cut -d: -f1)
check_line=$(awk -v s="$skip_line" 'NR < s && /^    check_gcc_thread_model$/ {n=NR} END {print n+0}' "$build")
[ -n "$skip_line" ] && [ "$check_line" -gt 0 ] && [ $((skip_line - check_line)) -le 3 ] \
    && ok "model check runs right before the early return (line $check_line, return at $skip_line)" \
    || fail "no model check right before the early 'already installed' return (check $check_line, return $skip_line)"
grep -q 'ppu_configured_threads "\$obj")" == "\$threads_mode"' "$build" && ok "configure argv is asserted after configure" \
    || fail "no assertion of the configured thread model after configure"

[ $status -eq 0 ] && echo "PASS: ppu-thread-model-test" || echo "FAIL: ppu-thread-model-test"
exit $status
