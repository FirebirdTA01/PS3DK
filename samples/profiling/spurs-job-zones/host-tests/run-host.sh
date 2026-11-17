#!/usr/bin/env bash
# Native Linux CI driver. Explicit output path; no target build or runtime.
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/build/directory" >&2
    exit 2
fi
case "$1" in /*) ;; *) echo "output directory must be absolute" >&2; exit 2 ;; esac
sj_tests=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sj_out=$1
sj_cc=${CC:-cc}
mkdir -p -- "$sj_out"
python3 -B "$sj_tests/test_verify.py"
"$sj_cc" -std=c11 -O2 -Wall -Wextra -Werror "$sj_tests/lifetime.c" -o "$sj_out/lifetime-test"
"$sj_out/lifetime-test"
echo "SPURS receipt verifier and native lifetime predicate passed; no target execution claimed"
