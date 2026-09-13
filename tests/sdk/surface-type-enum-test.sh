#!/usr/bin/env bash
# A render target's `type` field takes the SURFACE type family:
#
#     GCM_SURFACE_TYPE_LINEAR   1   (CELL_GCM_SURFACE_PITCH)
#     GCM_SURFACE_TYPE_SWIZZLE  2   (CELL_GCM_SURFACE_SWIZZLE)
#
# `GCM_TEXTURE_LINEAR` is 2 and belongs to the TEXTURE FILTER family. Assigning
# it to a surface type therefore says SWIZZLE, and `rsxSetSurface` ORs that value
# into NV40TCL_RT_FORMAT alongside log2Width/log2Height - so the RSX is told the
# render target is swizzled with power-of-two dimensions while being handed a
# linear pitch and offset.
#
# This compiled clean, ran clean under RPCS3, and hung a real PS3 on a black
# screen at the first flip after the surface was bound. 2026-09-09: 23 sample
# files carried it. Changing that one line - and nothing else - took the
# diagnostic from `TIMEOUT flip-status` to `CLEAR_PROBE_COMPLETE frames=360` on
# the same console.
#
# Both spellings are correct and both appear in the tree; what is never correct
# is a value from the texture family in a surface type field.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
status=0

fail() { echo "surface-type-enum: FAIL: $*" >&2; status=1; }
note() { echo "surface-type-enum: $*"; }

# `.type = GCM_TEXTURE_...` or `->type = GCM_TEXTURE_...`, in our own sources.
# samples/PSL1GHT, src/forks and src/ps3dev are vendored upstream copies: they
# carry the same defect, and correcting them here would silently diverge our
# copy from the upstream it exists to mirror. Out of scope for this check, not
# out of mind - upstream PSL1GHT has this bug too.
scan() {
    grep -rnE '(\.|->)type[[:space:]]*=[[:space:]]*GCM_TEXTURE_' \
        --include='*.c' --include='*.cpp' --include='*.h' \
        "$1/samples" "$1/sdk" "$1/runtime" "$1/src" 2>/dev/null \
        | grep -vE '/samples/PSL1GHT/|/src/forks/|/src/ps3dev/' || true
}

hits=$(scan "$root")
if [ -n "$hits" ]; then
    echo "$hits" | sed 's/^/  /' >&2
    fail "a surface type is assigned from the GCM_TEXTURE_* family (see above)"
else
    note "ok: no surface type takes a texture-family constant"
fi

# The check must be able to fail. Re-run it against a tree carrying the exact
# defect that hung the console, and require rejection.
selftest=$(mktemp -d)
trap 'rm -rf "$selftest"' EXIT
mkdir -p "$selftest/samples/probe/source" "$selftest/sdk" "$selftest/runtime" "$selftest/src"
cat > "$selftest/samples/probe/source/main.c" <<'PROBE'
static void set_render_target(void)
{
	gcmSurface sf = {0};
	sf.depthPitch       = depth_pitch;
	sf.type             = GCM_TEXTURE_LINEAR;
	sf.antiAlias        = GCM_SURFACE_CENTER_1;
}
PROBE
if [ -n "$(scan "$selftest")" ]; then
    note "self-test ok: the historical defect is still detected"
else
    fail "self-test: the check did not detect the exact line that hung the console"
fi

# And it must not fire on the correct spellings, or it would just be noise.
cat > "$selftest/samples/probe/source/main.c" <<'GOOD'
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	other.type = CELL_GCM_SURFACE_PITCH;
	sw.type = GCM_SURFACE_TYPE_SWIZZLE;
	tex.filter = GCM_TEXTURE_LINEAR;   /* a texture filter, which is fine */
GOOD
if [ -n "$(scan "$selftest")" ]; then
    fail "self-test: the check fires on correct code, including a legitimate texture filter"
else
    note "self-test ok: correct spellings and real texture filters pass"
fi

exit $status
