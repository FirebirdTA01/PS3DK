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
#   2. The new cellGcmSetFragmentProgramControl(CGprogram,...) family
#      (Safe, Unsafe, and the no-context C++ overload) lives in the
#      Cg-program bridge (<cell/gcm/gcm_cg_bridge.h>) alongside the
#      other Cg-handle fragment-program emitters, keeps deriving the
#      NV40 FP_CONTROL word through the ONE builder
#      (ps3tc_fp_control_word) rather than re-deriving it, and emits
#      the NV40TCL_FP_CONTROL command.  The old rsx-struct-signature
#      form is not there anymore.
#
# The `location` argument (new on setUpdateFragmentProgramParameter)
# passes through to the rsx emitter rather than being dropped.
#
# The full header is not host-compilable (PowerPC pointer-mode attributes),
# so this is static; the rsx emitters they forward to are compile-checked
# by the PPU-target tests when a toolchain is present.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
hdr="$root/sdk/include/cell/gcm/gcm_command_c.h"
overloads="$root/sdk/include/cell/gcm/gcm_cmd_overloads.h"
bridge="$root/sdk/include/cell/gcm/gcm_cg_bridge.h"

fail() { echo "gcm-command-surface: FAIL $*" >&2; exit 1; }
ok()   { echo "gcm-command-surface: ok   $*"; }

[ -f "$hdr" ]       || fail "gcm_command_c.h missing"
[ -f "$overloads" ] || fail "gcm_cmd_overloads.h missing"
[ -f "$bridge" ]    || fail "gcm_cg_bridge.h missing"

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

# --- 3. new fragment-program-control wrapper (CGprogram form) ----------
# The canonical API takes a Cg program handle (not an rsx struct); it
# belongs in the Cg bridge alongside cellGcmSetFragmentProgram, and
# MUST keep using the single FP_CONTROL builder (ps3tc_fp_control_word)
# rather than re-deriving the word.
grep -Eq 'static inline void +cellGcmSetFragmentProgramControl\(CellGcmContextData \*thisContext,' "$bridge" \
    || fail "cellGcmSetFragmentProgramControl(ctx, prog, controlTxp, reserved0, reserved1) - the new wrapper - is missing from gcm_cg_bridge.h"
grep -A2 'cellGcmSetFragmentProgramControl(CellGcmContextData \*thisContext,' "$bridge" \
    | grep -q 'CGprogram prog,' \
    || fail "cellGcmSetFragmentProgramControl does not take a CGprogram handle"
# it keeps using the ONE builder (no re-derived control word)
grep -A20 'cellGcmSetFragmentProgramControl(CellGcmContextData \*thisContext,' "$bridge" \
    | grep -q 'ps3tc_fp_control_word(' \
    || fail "cellGcmSetFragmentProgramControl does not use ps3tc_fp_control_word - it would be a second FP_CONTROL word source"
# and it still emits the NV40 FP_CONTROL command
grep -A24 'cellGcmSetFragmentProgramControl(CellGcmContextData \*thisContext,' "$bridge" \
    | grep -q 'PS3TC_GCM_METHOD(NV40TCL_FP_CONTROL, 1)' \
    || fail "cellGcmSetFragmentProgramControl does not emit NV40TCL_FP_CONTROL"
ok "cellGcmSetFragmentProgramControl(ctx, CGprogram, ...) in the bridge, uses the one builder, emits NV40TCL_FP_CONTROL"

grep -Eq 'static inline void +cellGcmSetFragmentProgramControlUnsafe\(CellGcmContextData \*thisContext,' "$bridge" \
    || fail "cellGcmSetFragmentProgramControlUnsafe - the new wrapper's Unsafe twin - is missing from gcm_cg_bridge.h"
ok "cellGcmSetFragmentProgramControlUnsafe present in the bridge"

# The old rsx-struct-signature form must NOT remain in gcm_command_c.h
# (it was moved to the Cg-handle bridge per the canonical API).
grep -q 'cellGcmSetFragmentProgramControl.*rsxFragmentProgram' "$hdr" \
    && fail "gcm_command_c.h still carries the rsxFragmentProgram-signature cellGcmSetFragmentProgramControl - it should be only in gcm_cg_bridge.h (CGprogram form)" \
    || ok "no rsx-struct cellGcmSetFragmentProgramControl left in gcm_command_c.h"

# --- 4. no-context C++ overloads (platform-ABI convention) -------------
grep -q 'static inline void cellGcmSetUpdateFragmentProgramParameterLocation(uint32_t offset, uint32_t location)' "$overloads" \
    || fail "gcm_cmd_overloads.h is missing the no-context cellGcmSetUpdateFragmentProgramParameterLocation(offset, location) overload"
grep -q 'static inline void cellGcmSetUpdateFragmentProgramParameter(uint32_t offset)' "$overloads" \
    || fail "gcm_cmd_overloads.h lost the no-context cellGcmSetUpdateFragmentProgramParameter(offset) overload (existing callers break)"
# the new FP-control no-context overload lives in the bridge (CGprogram form)
grep -q 'static inline void cellGcmSetFragmentProgramControl(CGprogram prog,' "$bridge" \
    || fail "gcm_cg_bridge.h is missing the no-context cellGcmSetFragmentProgramControl(CGprogram, ...) overload"
ok "no-context C++ overloads present (and the old one preserved)"

echo "gcm-command-surface: PASS"
