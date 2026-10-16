/* tests/sdk/np-drm-inline-test.c
 *
 * Compile-check for <np/drm_inline.h>.
 *
 * Includes the inline header and calls sceNpDrmOpen from an exported
 * function. Calling it makes the compiler emit the inline body, and that
 * body references the three external symbols (cellFsStat,
 * sceNpDrmIsAvailable, cellFsOpen) — so the linker is left with real
 * undefined references to each of them.
 *
 * The self-check in the .sh sed-rewrites cellFsOpen to a misspelling in
 * a copy of the header; with this TU, the call then pulls in a symbol
 * that doesn't exist, which is exactly what should fail.
 *
 * main() calls the probe so the object links against the stub archives.
 */
#include <np/drm_inline.h>

int np_drm_inline_probe(const char *path, int flags)
{
    int fd = -1;
    return sceNpDrmOpen(0, path, flags, &fd, 0, 0);
}

int main(void)
{
    return np_drm_inline_probe("/dev_hdd0/missing", 0) == 0;
}
