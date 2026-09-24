#!/usr/bin/env bash
# t_38e8bf5a: the GCM FIFO wrap must not lose a lap that was never published.
#
# Builds the host model (fifo-wrap-protocol-test.c) against the real protocol
# header, which must pass every scenario, and again with -DONE_PHASE_CONTROL,
# the one-phase protocol it replaced, which must FAIL the idle never-flushed
# lap.  The second build is the proof that the model can see the defect; a
# model that passed both would prove nothing.  Exit status is asserted exactly
# (1 for the control), so a crash cannot pass for a detected failure.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
src="$repo_root/tests/sdk/fifo-wrap-protocol-test.c"
inc="$repo_root/sdk/libgcm_cmd/src"
cc="${CC:-cc}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

command -v "$cc" >/dev/null 2>&1 || fail "no host C compiler ($cc)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

san=()
if "$cc" -fsanitize=address,undefined -x c /dev/null -c -o "$tmp/probe.o" >/dev/null 2>&1; then
    san=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi

"$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$inc" "$src" -o "$tmp/fixed"
"$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$inc" -DONE_PHASE_CONTROL "$src" -o "$tmp/control"

rc=0; "$tmp/fixed" || rc=$?
[[ "$rc" -eq 0 ]] || fail "two-phase protocol: expected exit 0, got $rc"

rc=0; "$tmp/control" >"$tmp/control.log" 2>&1 || rc=$?
cat "$tmp/control.log"
[[ "$rc" -eq 1 ]] || fail "one-phase control: expected exit 1 (the model must see the lost lap), got $rc"
grep -q 'idle, never-flushed lap (EMP E5)  *FAIL' "$tmp/control.log" \
    || fail "one-phase control failed, but not on the idle never-flushed lap"

echo "fifo-wrap-protocol-test: PASS"
