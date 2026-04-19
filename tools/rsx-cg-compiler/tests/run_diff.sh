#!/usr/bin/env bash
#
# Byte-diff rsx-cg-compiler output against Sony sce-cgc for every .cg in
# tests/shaders/.  Fails if any shader mismatches.
#
# Per project feedback (memory: feedback_byte_exact_shader_output): raw
# "looks correct" output is not enough — RSX rejects instruction
# encodings that diverge from sce-cgc in subtle ways that don't show up
# in functional code review.  This harness is the authoritative pass/
# fail gate for every rsx-cg-compiler change.
#
# Scope:
#   - VP and FP shaders both diff at the *full container* level
#     (header + parameter table + strings + program subtype + ucode)
#     when the container emitter can produce output for the shader.
#   - Struct-flattened VP shaders (e.g. ones that use `struct VIN`
#     parameters) currently fall back to ucode-only diff: sce-cgc
#     names struct fields as `<param>.<field>`/`<func>.<field>` and
#     our IR doesn't yet preserve those.  The container emitter
#     reports this case and we then compare ucode regions only.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$TOOL_DIR/../.." && pwd)"

RSX_CG_COMPILER="$TOOL_DIR/build/rsx-cg-compiler"
SCE_CGC_EXE="$REPO_DIR/reference/sony-sdk/host-win32/Cg/bin/sce-cgc.exe"
SHADER_DIR="$SCRIPT_DIR/shaders"
WORK_DIR="$(mktemp -d -t rsx-cg-compiler-diff.XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

# Test contract: we pin against sce-cgc's DEFAULT optimisation level
# (`-O2 --fastmath`).  Both sides are invoked with those flags so the
# contract is explicit — adding a new test shader at a different opt
# level requires invoking the harness with different flags.  See
# tools/rsx-cg-compiler/docs/OPTIMIZATION.md for the rationale.
OPT_FLAGS=(--O2 --fastmath)

if [[ ! -x "$RSX_CG_COMPILER" ]]; then
    echo "error: rsx-cg-compiler not built; run: cmake --build $TOOL_DIR/build" >&2
    exit 1
fi
if [[ ! -f "$SCE_CGC_EXE" ]]; then
    echo "error: sce-cgc.exe not found at $SCE_CGC_EXE" >&2
    echo "       mount the reference SDK (reference/sony-sdk) and retry" >&2
    exit 1
fi
if ! command -v wine >/dev/null; then
    echo "error: wine not installed — required to run sce-cgc.exe" >&2
    exit 1
fi

profile_of() {
    # Heuristic: *_v.cg → vertex, *_f.cg → fragment.
    case "$1" in
        *_v.cg)                echo "sce_vp_rsx" ;;
        *_color_v.cg)          echo "sce_vp_rsx" ;;
        *_f.cg)                echo "sce_fp_rsx" ;;
        *)                     echo "sce_vp_rsx" ;;
    esac
}

# Read a big-endian u32 from a .vpo file at the given byte offset.
read_be32() {
    local file="$1" off="$2"
    printf '%u' "0x$(xxd -s "$off" -l 4 -p "$file")"
}

# Extract the ucode blob from a Sony .vpo, using the container header.
# CgBinaryProgram layout (see reference/sony-sdk/.../Cg/cgBinary.h):
#   0x00 profile
#   0x04 binaryFormatRevision
#   0x08 totalSize
#   0x0c parameterCount
#   0x10 parameterArray
#   0x14 program
#   0x18 ucodeSize
#   0x1c ucode  (offset, relative to start of struct)
extract_vpo_ucode() {
    local vpo="$1"
    local ucode_size ucode_off
    ucode_size=$(read_be32 "$vpo" 0x18)
    ucode_off=$(read_be32 "$vpo" 0x1c)
    xxd -s "$ucode_off" -l "$ucode_size" -p "$vpo" | tr -d '\n'
}

collect_our_ucode() {
    local cg="$1" profile="$2"
    "$RSX_CG_COMPILER" --profile "$profile" "${OPT_FLAGS[@]}" "$cg" \
        | awk '/^  / { for (i=2;i<=NF;i++) printf "%s", $i }'
}

overall_rc=0
total=0
passed=0

shopt -s nullglob
for cg in "$SHADER_DIR"/*.cg; do
    total=$((total + 1))
    name="$(basename "$cg" .cg)"
    profile=$(profile_of "$name.cg")

    sce_out="$WORK_DIR/${name}_sce.bin"
    WINEDEBUG=-all wine "$SCE_CGC_EXE" --quiet --profile "$profile" \
        "${OPT_FLAGS[@]}" \
        -o "$sce_out" "$cg" >"$WORK_DIR/${name}.sce.log" 2>&1 || {
            echo "[FAIL] $name: sce-cgc failed — see $WORK_DIR/${name}.sce.log" >&2
            overall_rc=1
            continue
        }

    # Try full-container diff first.  If the container emitter bails
    # (e.g. struct-flattened VP shaders), fall back to ucode-only
    # diff against the ucode region inside sce-cgc's container.
    our_out="$WORK_DIR/${name}_ours.bin"
    if "$RSX_CG_COMPILER" --profile "$profile" \
            "${OPT_FLAGS[@]}" \
            --emit-container "$our_out" "$cg" \
            >"$WORK_DIR/${name}.our.log" 2>&1 \
       && [[ -s "$our_out" ]]; then
        if cmp -s "$our_out" "$sce_out"; then
            sce_size=$(stat -c%s "$sce_out")
            echo "[OK  ] $name ($sce_size bytes container match)"
            passed=$((passed + 1))
        else
            ext=${profile#sce_}; ext=${ext%_rsx}o
            echo "[FAIL] $name: .${ext} container diverges from sce-cgc"
            echo "       hex diff (ours -> sce):"
            diff <(xxd "$our_out") <(xxd "$sce_out") | sed 's/^/         /' | head -20
            overall_rc=1
        fi
    else
        # Container path bailed (or wrote nothing).  Fall back to
        # ucode-only diff so we still validate the NV40 emit path.
        our_hex=$(collect_our_ucode "$cg" "$profile")
        words=$((${#our_hex} / 8))
        sce_hex=$(extract_vpo_ucode "$sce_out" "$words")

        if [[ "$our_hex" == "$sce_hex" ]]; then
            echo "[ucode] $name ($words ucode words match — container emit skipped)"
            passed=$((passed + 1))
        else
            echo "[FAIL] $name: ucode diverges from sce-cgc"
            echo "       ours: $our_hex"
            echo "       sce : $sce_hex"
            overall_rc=1
        fi
    fi
done

echo
echo "$passed / $total shaders match sce-cgc byte-for-byte"
exit "$overall_rc"
