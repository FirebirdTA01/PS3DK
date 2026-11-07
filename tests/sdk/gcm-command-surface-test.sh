#!/usr/bin/env bash
# Pins the command-emitter surface added for the missing cellGcm* wrappers
# (t_4c9e5fd7) over <cell/gcm/gcm_command_c.h>, at the structural level the
# host can check.  Two invariants the CI PPU-target compile tests
# (gcm-redefinition-test.sh, gcm-cg-resource-test.sh - both SKIP without a
# PS3DEV prefix) do NOT pin on their own:
#
#   1. The original 2-arg form cellGcmSetUpdateFragmentProgramParameter(ctx,
#      offset) MUST still exist and still pin the location to local memory
#      (GCM_LOCATION_RSX).  It is called from gcm_cmd_overloads.h and
#      gcm_command_cpp_explicit.h; changing its signature silently breaks
#      their callers.  The new behaviour lives in the separate
#      ...ParameterLocation(ctx, offset, location) form instead.
#
#   2. Each new wrapper stays a THIN forwarder to its rsx* emitter (the
#      single source of truth for the NV40 command word - see
#      gcm_fp_control.h's "one builder" comment and the repo's ban on
#      duplicating the arithmetic), and the new `location` argument is
#      actually passed through rather than dropped.
#
# The full header is not host-compilable (PowerPC pointer-mode attributes),
# so this is static; the rsx emitters they forward to are compile-checked
# by the PPU-target tests when a toolchain is present.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
hdr="$root/sdk/include/cell/gcm/gcm_command_c.h"
overloads="$root/sdk/include/cell/gcm/gcm_cmd_overloads.h"

fail() { echo "gcm-command-surface: FAIL $*" >&2; exit 1; }
ok()   { echo "gcm-command-surface: ok   $*"; }

[ -f "$hdr" ]       || fail "gcm_command_c.h missing"
[ -f "$overloads" ] || fail "gcm_cmd_overloads.h missing"

# --- 1. backward-compat: the 2-arg form is preserved --------------------
grep -Eq 'static inline void +cellGcmSetUpdateFragmentProgramParameter\(CellGcmContextData \*thisContext, uint32_t offset\)' "$hdr" \
    || fail "the 2-arg cellGcmSetUpdateFragmentProgramParameter(ctx, offset) form is gone - its callers in gcm_cmd_overloads.h / gcm_command_cpp_explicit.h break"
ok "2-arg cellGcmSetUpdateFragmentProgramParameter(ctx, offset) preserved"

# The 2-arg form still pins the location to local memory and delegates.
# The call lives on the line(s) right after the 2-arg signature.
grep -A2 'cellGcmSetUpdateFragmentProgramParameter(CellGcmContextData \*thisContext, uint32_t offset)' "$hdr" \
    | grep -q 'rsxUpdateFragmentProgramLocation(thisContext, offset, GCM_LOCATION_RSX);' \
    || fail "the 2-arg form no longer forwards to rsxUpdateFragmentProgramLocation pinned to GCM_LOCATION_RSX - the old behaviour moved"
ok "2-arg form still delegates to rsxUpdateFragmentProgramLocation(ctx, offset, GCM_LOCATION_RSX)"

# --- 2. new location-argument form -------------------------------------
grep -Eq 'static inline void +cellGcmSetUpdateFragmentProgramParameterLocation\(CellGcmContextData \*thisContext,' "$hdr" \
    || fail "cellGcmSetUpdateFragmentProgramParameterLocation(ctx, offset, location) - the new location-argument form - is missing"
# its signature takes (offset, location)
grep -A3 'cellGcmSetUpdateFragmentProgramParameterLocation(CellGcmContextData \*thisContext,' "$hdr" \
    | grep -Eq 'uint32_t +location\)' \
    || fail "the new location-argument form does not take (offset, location)"
# the `location` argument is actually forwarded (not dropped)
grep -A6 'cellGcmSetUpdateFragmentProgramParameterLocation(CellGcmContextData \*thisContext,' "$hdr" \
    | grep -q 'rsxUpdateFragmentProgramLocation(thisContext, offset, location);' \
    || fail "the new form does not pass `location` through to rsxUpdateFragmentProgramLocation - the location arg is being dropped"
ok "cellGcmSetUpdateFragmentProgramParameterLocation(ctx, offset, location) forwards location to rsx"

grep -Eq 'static inline void +cellGcmSetUpdateFragmentProgramParameterLocationUnsafe\(CellGcmContextData \*thisContext,' "$hdr" \
    || fail "cellGcmSetUpdateFragmentProgramParameterLocationUnsafe - the new form's Unsafe twin - is missing"
grep -A6 'cellGcmSetUpdateFragmentProgramParameterLocationUnsafe(CellGcmContextData \*thisContext,' "$hdr" \
    | grep -q 'rsxUpdateFragmentProgramLocationUnsafe(thisContext, offset, location);' \
    || fail "the Unsafe location form does not pass `location` through to rsxUpdateFragmentProgramLocationUnsafe"
ok "cellGcmSetUpdateFragmentProgramParameterLocationUnsafe forwards location to rsx"

# --- 3. new fragment-program-control wrapper (safe + Unsafe) ------------
grep -Eq 'static inline void +cellGcmSetFragmentProgramControl\(CellGcmContextData \*thisContext,' "$hdr" \
    || fail "cellGcmSetFragmentProgramControl(ctx, program, allowTex, reserved0, reserved1) - the new wrapper - is missing"
grep -A6 'cellGcmSetFragmentProgramControl(CellGcmContextData \*thisContext,' "$hdr" \
    | grep -q 'rsxSetFragmentProgramControl(thisContext, program, allowTex, reserved0, reserved1);' \
    || fail "cellGcmSetFragmentProgramControl is no longer a thin forwarder to rsxSetFragmentProgramControl - it would be a second control-word source"
ok "cellGcmSetFragmentProgramControl is a thin forwarder to rsxSetFragmentProgramControl"

grep -Eq 'static inline void +cellGcmSetFragmentProgramControlUnsafe\(CellGcmContextData \*thisContext,' "$hdr" \
    || fail "cellGcmSetFragmentProgramControlUnsafe - the new wrapper's Unsafe twin - is missing"
grep -A6 'cellGcmSetFragmentProgramControlUnsafe(CellGcmContextData \*thisContext,' "$hdr" \
    | grep -q 'rsxSetFragmentProgramControlUnsafe(thisContext, program, allowTex, reserved0, reserved1);' \
    || fail "cellGcmSetFragmentProgramControlUnsafe is no longer a thin forwarder to rsxSetFragmentProgramControlUnsafe"
ok "cellGcmSetFragmentProgramControlUnsafe is a thin forwarder to rsx"

# --- 4. no-context C++ overloads (platform-ABI convention) -------------
grep -q 'static inline void cellGcmSetUpdateFragmentProgramParameterLocation(uint32_t offset, uint32_t location)' "$overloads" \
    || fail "gcm_cmd_overloads.h is missing the no-context cellGcmSetUpdateFragmentProgramParameterLocation(offset, location) overload"
grep -q 'static inline void cellGcmSetUpdateFragmentProgramParameter(uint32_t offset)' "$overloads" \
    || fail "gcm_cmd_overloads.h lost the no-context cellGcmSetUpdateFragmentProgramParameter(offset) overload (existing callers break)"
grep -q 'static inline void cellGcmSetFragmentProgramControl(const rsxFragmentProgram' "$overloads" \
    || fail "gcm_cmd_overloads.h is missing the no-context cellGcmSetFragmentProgramControl overload"
ok "no-context C++ overloads present (and the old one preserved)"

echo "gcm-command-surface: PASS"
