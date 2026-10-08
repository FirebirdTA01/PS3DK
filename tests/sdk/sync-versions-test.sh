#!/usr/bin/env bash
# scripts/sync-versions.sh keeps tools/VERSION exactly "X.Y.Z\n".
#
# The Rust tools embed tools/VERSION and trim only trailing whitespace, so the
# script must not call a file in sync that the tools would read differently.
# Every row runs a copy of the script on a scratch tree with --version=0.20.6
# (no git needed):
#   exact        "0.20.6\n": --check passes, write prints nothing, file kept.
#   leading      " 0.20.6\n"     --check fails; write makes it "0.20.6\n".
#   internal     "0.20\n.6\n"    --check fails; write makes it "0.20.6\n".
#   no newline   "0.20.6"        --check fails; write makes it "0.20.6\n".
#   crlf         "0.20.6\r\n"    --check fails; write makes it "0.20.6\n".
#   older        "0.20.5\n"      --check fails; --dry-run reports and keeps
#                                the file; write makes it "0.20.6\n".
#   bad flag     --version=1.2 is refused (exit 1), file kept.
#   missing      no tools/VERSION: exit 1.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
script="${1:-$repo_root/scripts/sync-versions.sh}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -f "$script" ]] || fail "no script at $script"

work="$(mktemp -d "${TMPDIR:-/tmp}/sync-versions.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/scripts" "$work/tools"
cp "$script" "$work/scripts/sync-versions.sh"
printf '0.20.6\n' > "$work/want"

run() {  # <args...>: prints the exit status; output to $work/out
    local rc=0
    bash "$work/scripts/sync-versions.sh" "$@" > "$work/out" 2>&1 || rc=$?
    printf '%s' "$rc"
}

# ---- exact ------------------------------------------------------------------
printf '0.20.6\n' > "$work/tools/VERSION"
rc="$(run --version=0.20.6 --check)"
[[ "$rc" -eq 0 ]] || fail "exact: --check exit $rc ($(head -1 "$work/out"))"
rc="$(run --version=0.20.6)"
[[ "$rc" -eq 0 && ! -s "$work/out" ]] || fail "exact: write exit $rc, output '$(head -1 "$work/out")'"
cmp -s "$work/tools/VERSION" "$work/want" || fail "exact: write changed the file"
echo "  ok: exact: in sync, write is a silent no-op"

# ---- malformed forms of the right number --------------------------------------
row() {  # <name> <printf format for the file>
    local name="$1" fmt="$2"
    printf "$fmt" > "$work/tools/VERSION"
    cp "$work/tools/VERSION" "$work/before"
    rc="$(run --version=0.20.6 --check)"
    [[ "$rc" -eq 1 ]] || fail "$name: --check exit $rc, expected 1"
    cmp -s "$work/tools/VERSION" "$work/before" || fail "$name: --check changed the file"
    rc="$(run --version=0.20.6)"
    [[ "$rc" -eq 0 ]] || fail "$name: write exit $rc ($(head -1 "$work/out"))"
    cmp -s "$work/tools/VERSION" "$work/want" || fail "$name: write left $(od -An -c "$work/tools/VERSION")"
    rc="$(run --version=0.20.6 --check)"
    [[ "$rc" -eq 0 ]] || fail "$name: --check after write exit $rc"
    echo "  ok: $name: --check refuses, write canonicalizes"
}
row leading    ' 0.20.6\n'
row internal   '0.20\n.6\n'
row no-newline '0.20.6'
row crlf       '0.20.6\r\n'

# ---- older version ------------------------------------------------------------
printf '0.20.5\n' > "$work/tools/VERSION"
rc="$(run --version=0.20.6 --check)"
[[ "$rc" -eq 1 ]] || fail "older: --check exit $rc, expected 1"
rc="$(run --version=0.20.6 --dry-run)"
[[ "$rc" -eq 0 ]] && grep -q 'would rewrite tools/VERSION' "$work/out" || fail "older: --dry-run exit $rc ($(head -1 "$work/out"))"
grep -qx '0.20.5' "$work/tools/VERSION" || fail "older: --dry-run changed the file"
rc="$(run --version=0.20.6)"
[[ "$rc" -eq 0 ]] && cmp -s "$work/tools/VERSION" "$work/want" || fail "older: write exit $rc"
echo "  ok: older: --check refuses, --dry-run keeps the file, write updates it"

# ---- bad flag, missing file -------------------------------------------------------
rc="$(run --version=1.2)"
[[ "$rc" -eq 1 ]] && cmp -s "$work/tools/VERSION" "$work/want" || fail "bad flag: exit $rc or file changed"
rm "$work/tools/VERSION"
rc="$(run --version=0.20.6 --check)"
[[ "$rc" -eq 1 ]] || fail "missing: exit $rc, expected 1"
echo "  ok: a malformed --version and a missing file are refused"

echo "sync-versions: PASS"
