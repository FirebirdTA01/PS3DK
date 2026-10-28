#!/usr/bin/env bash
# ps3_add_prx forwards NAME / VERSION / ATTRIBUTES to `prx-gen build' only
# when they were supplied, and must forward any supplied value verbatim -
# including strings CMake treats as false (OFF, NO, 0, N, FALSE, IGNORE),
# so prx-gen can compare them with the module's SYS_MODULE_INFO record and
# validate malformed values instead of silently losing them.  Runs the
# helper in cmake/ps3-prx-identity.cmake in script mode; needs only cmake.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
command -v cmake > /dev/null || { echo "prx-identity-args: SKIP (no cmake)"; exit 0; }
helper="$root/cmake/ps3-prx-identity.cmake"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
status=0; n=0
check() {  # check <label> <expected ARGS> <cmake set() lines...>
    local label=$1 want=$2; shift 2
    { printf 'include("%s")\n' "$helper"
      printf '%s\n' "$@"
      printf '_ps3_prx_identity_args(args _T)\nmessage("ARGS=${args}")\n'; } > "$work/t.cmake"
    local got
    got=$(cmake -P "$work/t.cmake" 2>&1 | tr -d '\r' | sed -n 's/^ARGS=//p')
    if [ "$got" = "$want" ]; then
        echo "prx-identity-args: ok   $label"; n=$((n + 1))
    else
        echo "prx-identity-args: FAIL: $label: got '$got', want '$want'"; status=1
    fi
}
check "all three given" "--name;mod;--version;3.9;--attributes;5" \
    'set(_T_NAME mod)' 'set(_T_VERSION 3.9)' 'set(_T_ATTRIBUTES 5)'
check "nothing given" "" 'unset(_T_NAME)'
check "empty values are not supplied" "" 'set(_T_NAME "")' 'set(_T_VERSION "")' 'set(_T_ATTRIBUTES "")'
for v in OFF NO N FALSE IGNORE NOTFOUND 0; do
    check "NAME $v is forwarded" "--name;$v" "set(_T_NAME $v)"
done
check "VERSION 0 is forwarded for prx-gen to reject" "--version;0" 'set(_T_VERSION 0)'
check "ATTRIBUTES 0 is forwarded" "--attributes;0" 'set(_T_ATTRIBUTES 0)'
[ $status -eq 0 ] && echo "prx-identity-args: PASS ($n checks)" || echo "prx-identity-args: FAIL"
exit $status
