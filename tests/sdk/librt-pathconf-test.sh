#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -Dpathconf=ps3tc_pathconf \
    "$repo_root/tests/sdk/librt-pathconf-test.c" \
    "${PATHCONF_SOURCE:-$repo_root/runtime/lv2/librt/pathconf.c}" \
    -o "$work/test"
"$work/test" "$work" "$work/missing"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -Dpathconf=ps3tc_pathconf -DMODEL_LV2_STAT \
    "$repo_root/tests/sdk/librt-pathconf-test.c" \
    "${PATHCONF_SOURCE:-$repo_root/runtime/lv2/librt/pathconf.c}" \
    -Wl,--wrap=stat -o "$work/model-test"
"$work/model-test" "$work" "$work/missing"
