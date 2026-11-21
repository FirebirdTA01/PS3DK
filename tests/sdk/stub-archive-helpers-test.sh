#!/usr/bin/env bash
# scripts/lib/stub-archive-helpers.sh, and scripts/build-cell-stub-archives.sh's use of it, with no toolchain:
#  1. make_clean_all runs `make clean` and then `make all` as two invocations (a fake make records them), the
#     caller's variable assignments reach both, and a failing clean stops before all;
#  2. nidgen_bin follows CARGO_TARGET_DIR as cargo does (unset, absolute, relative to the current directory);
#  3. build-cell-stub-archives.sh has no single `make ... clean all` and no hard-coded nidgen target path.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
root="$(cd "$here/../.." && pwd -P)"
fails=0
fail() { printf 'FAIL %s\n' "$*"; fails=$((fails + 1)); }
tmp="$(mktemp -d)"
trap 'find "$tmp" -depth -delete' EXIT

# shellcheck source=../../scripts/lib/stub-archive-helpers.sh
source "$root/scripts/lib/stub-archive-helpers.sh"

mkdir -p "$tmp/bin"
cat > "$tmp/bin/make" <<'MAKE'
#!/usr/bin/env bash
printf '%s|%s\n' "$*" "${PROBE_VAR:-unset}" >> "$MAKE_LOG"
[[ "${MAKE_FAIL_GOAL:-}" == "${*: -1}" ]] && exit 2
exit 0
MAKE
chmod +x "$tmp/bin/make"
export PATH="$tmp/bin:$PATH" MAKE_LOG="$tmp/make.log"

: > "$MAKE_LOG"
PROBE_VAR=seen make_clean_all /some/dir
want=$'-C /some/dir clean|seen\n-C /some/dir all|seen'
[[ "$(cat "$MAKE_LOG")" == "$want" ]] || fail "make_clean_all: $(tr '\n' ';' < "$MAKE_LOG"), want two makes: clean then all"

: > "$MAKE_LOG"
if MAKE_FAIL_GOAL=clean make_clean_all /some/dir; then fail "make_clean_all: a failing clean was not reported"; fi
[[ "$(cat "$MAKE_LOG")" == "-C /some/dir clean|unset" ]] || fail "make_clean_all: all ran after a failing clean"

[[ "$(env -u CARGO_TARGET_DIR bash -c "source '$root/scripts/lib/stub-archive-helpers.sh'; nidgen_bin /r")" \
   == /r/tools/target/release/nidgen ]] || fail "nidgen_bin without CARGO_TARGET_DIR"
[[ "$(CARGO_TARGET_DIR=/t/abs nidgen_bin /r)" == /t/abs/release/nidgen ]] || fail "nidgen_bin, absolute CARGO_TARGET_DIR"
[[ "$(cd "$tmp" && CARGO_TARGET_DIR=rel nidgen_bin /r)" == "$tmp/rel/release/nidgen" ]] \
    || fail "nidgen_bin, relative CARGO_TARGET_DIR (cargo reads it from the current directory)"

script="$root/scripts/build-cell-stub-archives.sh"
if grep -nE '^[^#]*\bmake\b[^#]*\bclean\b[^#]*\ball\b' "$script"; then
    fail "build-cell-stub-archives.sh still runs clean and all in one make"
fi
if grep -n 'tools/target/release/nidgen' "$script"; then
    fail "build-cell-stub-archives.sh hard-codes the nidgen target path (CARGO_TARGET_DIR ignored)"
fi
grep -q 'nidgen_bin "\$PS3_TOOLCHAIN_ROOT"' "$script" || fail "build-cell-stub-archives.sh does not use nidgen_bin"

if (( fails )); then printf 'stub-archive-helpers-test: %d failure(s)\n' "$fails"; exit 1; fi
echo "stub-archive-helpers-test: PASS"
