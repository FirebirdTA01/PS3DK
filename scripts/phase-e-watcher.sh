#!/bin/bash
set -u
LOG=/home/firebirdta01/source/repos/PS3_Custom_Toolchain/build/phase-e-build.log
PRJ=PS3_Custom_Toolchain
FROM=claude_PS3DK
TO=@claude_PS3DK,@deepseek_PS3DK

# Wait for log file to exist
while [[ ! -f "$LOG" ]]; do sleep 2; done

# Reset milestone state at start
declare -A seen

tail -F -n 0 "$LOG" 2>/dev/null | while IFS= read -r line; do
  # FAILURE TRIPWIRES
  case "$line" in
    *'Patch '*' failed'*|*'configure: error'*|*'make: ***'*' Error '*|*'unable to recognize insn'*|*'internal compiler error'*|*'fatal error:'*)
      rdac-agent say --project "$PRJ" --from "$FROM" --to "$TO" \
        "Phase E build FAILURE detected. Failure-marker line: $line. See build/phase-e-build.log."
      exit 1
      ;;
  esac
  # MILESTONES (once each)
  for ms in 'Extracting releases/gcc-12.4.0' 'Configuring GCC' 'Building GCC+newlib' 'Installing GCC+newlib'; do
    case "$line" in
      *"$ms"*)
        if [[ -z "${seen[$ms]:-}" ]]; then
          seen[$ms]=1
          rdac-agent say --project "$PRJ" --from "$FROM" --to "$TO" \
            "Phase E milestone: $ms."
        fi
        ;;
    esac
  done
done
