/* sdk/libfontFT_legacy/src/fontft_revision.c
 *
 * cellFontFTGetStubRevisionFlags, which the inline
 * cellFontInitLibraryFreeType in <cell/font/libfontFT.h> calls. It is not a
 * firmware export: the stub archive supplies it. The value is the one the
 * reference SDK's stub archive reports.
 */

#include <stddef.h>
#include <stdint.h>

void cellFontFTGetStubRevisionFlags(uint64_t *revisionFlags);

void cellFontFTGetStubRevisionFlags(uint64_t *revisionFlags)
{
    if (revisionFlags == NULL)
        return;
    *revisionFlags = 0x42;
}
