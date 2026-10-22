/* PLANNED preprocess check of the PAL selection guard in sdk/libps3dkprof/src/pal/pal.h (codex 2026-10-22 03:21).
 * Not wired into any build, CI job or test list; running it needs its own frozen request (see EXPECTED.md).  The file
 * only includes the shared header, so the guard alone decides whether preprocessing succeeds. */
#include "../../../../sdk/libps3dkprof/src/pal/pal.h"

int pal_select_probe_marker;
