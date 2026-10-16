#!/usr/bin/env bash
# ci/shellcheck_ratchet.py fails closed at every step (codex 1fe28976).  Drives the ratchet against small fixture
# trees with a stub shellcheck that reports SC2086 for every "echo $" line (gcc format) and can be told to misbehave
# (STUB_MODE).  Needs no real shellcheck.  Rows, each with its expected exit and message:
#   clean tree, empty baseline .................... 0     finding within baseline ............... 0
#   finding in a NESTED script, not in baseline ... 1 NEW (and the nested script is enumerated)
#   count above baseline .......................... 1 REGRESSION     count below baseline ...... 0, listed as dropped
#   unreadable directory under scripts/ ........... 1 "cannot enumerate"   (skipped when run as root)
#   shellcheck exit 2 ............................. 1      unparsed output line with exit 1 ............. 1
#   exit 1 with no finding ........................ 1      anything on shellcheck's stderr .............. 1
#   blind shellcheck (self-check) ................. 1      malformed baseline line / duplicate pair ..... 1
#   failing find and awk first on PATH (the old pipeline's tools) do not change a NEW verdict
#   --write-baseline prints exactly the counted pairs
# usage: shellcheck-ratchet-test.sh   Exit 0 pass, 1 fail.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ratchet="$root/ci/shellcheck_ratchet.py"
work=$(mktemp -d); trap 'chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"' EXIT
status=0
ok()   { echo "shellcheck-ratchet: ok   $*"; }
fail() { echo "shellcheck-ratchet: FAIL $*"; status=1; }
mkdir -p "$work/bin" "$work/badtools"
cat > "$work/bin/shellcheck" <<'STUB'
#!/usr/bin/env bash
case "${STUB_MODE:-}" in
  exit2) echo "shellcheck: internal error"; exit 2 ;;
  blind) exit 0 ;;
esac
found=0
for a in "$@"; do
  case "$a" in -S|style|-f|gcc|--) continue ;; esac
  [ -f "$a" ] || continue
  n=0
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    case "$line" in *'echo $'*) echo "$a:$n:6: note: Double quote to prevent globbing and word splitting. [SC2086]"; found=1 ;; esac
  done < "$a"
done
case "${STUB_MODE:-}" in
  unparsed) echo "In scripts/x.sh line 3: something it could not format" ;;
  stderr) echo "shellcheck: warning: could not read config" >&2 ;;
  exit1-empty) exit 1 ;;
esac
exit $found
STUB
chmod +x "$work/bin/shellcheck"
printf '#!/bin/sh\nexit 1\n' > "$work/badtools/find"; printf '#!/bin/sh\nexit 2\n' > "$work/badtools/awk"; chmod +x "$work/badtools/find" "$work/badtools/awk"

# tree <dir> <baseline text> [<file>:<content> ...]
tree() {
    local d=$1 base=$2; shift 2
    rm -rf "$d"; mkdir -p "$d/scripts" "$d/ci"
    printf '%b' "$base" > "$d/ci/shellcheck-baseline.txt"
    printf '#!/usr/bin/env bash\necho "fine"\n' > "$d/scripts/a.sh"
    local spec; for spec in "$@"; do mkdir -p "$(dirname "$d/${spec%%:*}")"; printf '%s' "${spec#*:}" > "$d/${spec%%:*}"; done
}
# run <label> <expected exit> <regex the output must match> <dir> [env...]
run() {
    local label=$1 want=$2 re=$3 d=$4; shift 4
    local out rc
    out=$(env PATH="$work/bin:$PATH" "$@" python3 "$ratchet" --root "$d" 2>&1); rc=$?
    if [ $rc -eq "$want" ] && grep -qE -- "$re" <<< "$out"; then ok "$label (exit $rc)"
    else fail "$label: exit $rc (want $want)"; sed 's/^/    /' <<< "$out" | head -8; fi
}
F1=$'#!/usr/bin/env bash\necho $1\n'
F2=$'#!/usr/bin/env bash\necho $1\necho $2\n'
tree "$work/t1" '# empty\n';                                          run "clean tree, empty baseline" 0 'green' "$work/t1"
tree "$work/t2" 'scripts/a.sh SC2086 1\n' "scripts/a.sh:$F1";          run "finding within baseline" 0 '1 finding\(s\) within baseline' "$work/t2"
tree "$work/t3" '# empty\n' "scripts/sub/deep/b.sh:$F1";               run "nested script, new finding" 1 'NEW.*scripts/sub/deep/b.sh.*SC2086' "$work/t3"
tree "$work/t4" 'scripts/a.sh SC2086 1\n' "scripts/a.sh:$F2";          run "count above baseline" 1 'REGRESSION.*scripts/a.sh.*SC2086.*2' "$work/t4"
tree "$work/t5" 'scripts/a.sh SC2086 2\n' "scripts/a.sh:$F1";          run "count below baseline" 0 'dropped' "$work/t5"
if [ "$(id -u)" -ne 0 ]; then
    tree "$work/t6" '# empty\n' "scripts/locked/c.sh:$F1"; chmod 000 "$work/t6/scripts/locked"
    run "unreadable directory under scripts/" 1 'cannot enumerate' "$work/t6"; chmod 755 "$work/t6/scripts/locked"
else
    echo "shellcheck-ratchet: SKIP unreadable-directory row (running as root)"
fi
tree "$work/t7" '# empty\n' "scripts/a.sh:$F1"
run "shellcheck exit 2"          1 'exited 2'                 "$work/t7" STUB_MODE=exit2
run "unparsed output, exit 1"    1 'unparsed shellcheck output' "$work/t7" STUB_MODE=unparsed
run "anything on stderr"         1 'stderr'                   "$work/t7" STUB_MODE=stderr
tree "$work/t8" '# empty\n'
run "exit 1 with no finding"     1 'exited 1 .* with no finding' "$work/t8" STUB_MODE=exit1-empty
run "blind shellcheck"           1 'self-check blind'         "$work/t8" STUB_MODE=blind
tree "$work/t9" 'scripts/a.sh SC2086\n' "scripts/a.sh:$F1";            run "malformed baseline line" 1 "not '<file> <SCnnnn> <count>'" "$work/t9"
tree "$work/t10" 'scripts/a.sh SC2086 1\nscripts/a.sh SC2086 1\n' "scripts/a.sh:$F1"; run "duplicate baseline pair" 1 'duplicate pair' "$work/t10"
out=$(env PATH="$work/badtools:$work/bin:$PATH" python3 "$ratchet" --root "$work/t3" 2>&1); rc=$?
if [ $rc -eq 1 ] && grep -q 'NEW.*scripts/sub/deep/b.sh' <<< "$out"; then ok "failing find/awk on PATH do not change a NEW verdict (exit $rc)"
else fail "failing find/awk on PATH: exit $rc"; fi
tree "$work/t11" '# empty\n' "scripts/a.sh:$F2" "scripts/x/y.sh:$F1"
out=$(env PATH="$work/bin:$PATH" python3 "$ratchet" --root "$work/t11" --write-baseline 2>&1); rc=$?
if [ $rc -eq 0 ] && [ "$out" = $'scripts/a.sh SC2086 2\nscripts/x/y.sh SC2086 1' ]; then ok "--write-baseline prints the counted pairs"
else fail "--write-baseline: exit $rc"; sed 's/^/    /' <<< "$out"; fi
[ $status -eq 0 ] && echo "PASS: shellcheck-ratchet-test" || echo "FAIL: shellcheck-ratchet-test"
exit $status
