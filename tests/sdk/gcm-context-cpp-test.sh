#!/usr/bin/env bash
# cell::Gcm::CellGcmContext (sdk/include/cell/gcm/gcm_command_cpp_explicit.h)
# is generated from the GCM command headers: fail when a command was added
# or changed without regenerating it.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
python3 "$repo_root/scripts/gen-gcm-context-cpp.py" --check
