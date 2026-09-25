/* sdk/libfont_legacy/src/font_revision.c
 *
 * cellFontGetStubRevisionFlags, which the inline cellFontInit in
 * <cell/font/libfont.h> calls. It is not a firmware export: the stub
 * archive supplies it, telling the libfont PRX which stub revision the
 * program was linked against. The value is the one the reference SDK's
 * stub archive reports.
 */

#include <stddef.h>
#include <stdint.h>

void cellFontGetStubRevisionFlags(uint64_t *revisionFlags);

void cellFontGetStubRevisionFlags(uint64_t *revisionFlags)
{
    if (revisionFlags == NULL)
        return;
    *revisionFlags = 0x62;
}
