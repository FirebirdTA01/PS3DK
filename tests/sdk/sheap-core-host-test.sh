#!/usr/bin/env bash
# Host unit test for the portable libsheap core: builds
# sdk/libsheap/src/core/*.c with the host compiler and runs
# tests/sdk/sheap-core-host-test.c (layout, fence bits, allocation example,
# rightmost-node rule, Free errors, key-table state machine, random run
# against a bitmap model).  Also compiles the shared cell/sheap/sheap_types.h
# in C and C++ so its layout checks are exercised on a 64-bit host.
#
# usage: sheap-core-host-test.sh     (CC / CXX override the host compilers)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cc="${CC:-cc}"
cxx="${CXX:-c++}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

printf '#include <cell/sheap/sheap_types.h>\nint main(void) { return sizeof(CellKeySheapBuffer) != 32; }\n' > "$work/types.c"
"$cc" -std=gnu99 -Wall -Wextra -Werror -I"$root/sdk/include" "$work/types.c" -o "$work/types-c"
"$cc" -std=c11 -Wall -Wextra -Werror -I"$root/sdk/include" "$work/types.c" -o "$work/types-c11"
if command -v "$cxx" > /dev/null 2>&1; then
    cp "$work/types.c" "$work/types.cpp"
    "$cxx" -std=c++17 -Wall -Wextra -Werror -I"$root/sdk/include" "$work/types.cpp" -o "$work/types-cxx"
    "$work/types-cxx"
fi
"$work/types-c"
echo "sheap-core: shared sheap_types.h layout checks compile (C, C++)"

"$cc" -std=c99 -O2 -Wall -Wextra -Werror \
    -I"$root/sdk/include" -I"$root/sdk/libsheap/src" \
    "$root/sdk/libsheap/src/core/tree.c" \
    "$root/sdk/libsheap/src/core/heap.c" \
    "$root/tests/sdk/sheap-core-host-test.c" \
    -o "$work/sheap-core"
"$work/sheap-core"
