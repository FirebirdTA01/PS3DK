#!/usr/bin/env bash
# Host test: sys_ppu_thread_exit on a thread with no TLS area (librt and the
# module runtime), and the module's sys_ppu_thread_create start records.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd -P)
python3 "$root/tests/host/librt-thread-exit-test.py" "$@"
python3 "$root/tests/host/librt-thread-exit-test.py" --sanitize "$@"
