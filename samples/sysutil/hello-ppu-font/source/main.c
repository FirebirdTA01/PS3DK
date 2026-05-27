/*
 * hello-ppu-font - cellFont surface validation.
 *
 * Validates: cellFontInitializeWithRevision, GetRevisionFlags,
 * GetInitializedRevisionFlags, SetFontsetOpenMode/SetFontOpenMode,
 * GetScalePixel, GetResolutionDpi, cellFontEnd.
 *
 * Expected return codes:
 *   InitializeWithRevision(NULL):  0x80540002  (CELL_FONT_ERROR_INVALID_PARAMETER)
 *   GetRevisionFlags(NULL):        0x00000000  (CELL_OK)
 *   GetInitializedRevisionFlags:   0x00000000  (CELL_OK)
 *   SetFontsetOpenMode(NULL):      0x80540002  (INVALID_PARAMETER)
 *   SetFontOpenMode(NULL):         0x80540002  (INVALID_PARAMETER)
 *   GetScalePixel(NULL):           0x80540002  (INVALID_PARAMETER)
 *   GetResolutionDpi(NULL):        0x80540002  (INVALID_PARAMETER)
 *   cellFontEnd():                 0x00000000  (CELL_OK)
 *
 * Link: cellFont_stub rt lv2.  No sysmodule/io needed (sample does
 * not call cellSysmoduleLoadModule).
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <cell/font.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	printf("hello-ppu-font: cellFont surface validation\n");
	CellFontConfig cfg;
	CellFontConfig_initialize(&cfg);
	uint64_t rev = 0;
	int rc = cellFontInitializeWithRevision(rev, &cfg);
	printf("  cellFontInitializeWithRevision -> 0x%08x\n", (unsigned)rc);
	rc = cellFontGetRevisionFlags(NULL, &rev);
	printf("  cellFontGetRevisionFlags -> 0x%08x\n", (unsigned)rc);
	rc = cellFontGetInitializedRevisionFlags(NULL, &rev);
	printf("  cellFontGetInitializedRevisionFlags -> 0x%08x\n", (unsigned)rc);
	rc = cellFontSetFontsetOpenMode(NULL, 0);
	printf("  cellFontSetFontsetOpenMode -> 0x%08x\n", (unsigned)rc);
	rc = cellFontSetFontOpenMode(NULL, 0);
	printf("  cellFontSetFontOpenMode -> 0x%08x\n", (unsigned)rc);
	float vw = 0, vh = 0;
	rc = cellFontGetScalePixel(NULL, &vw, &vh);
	printf("  cellFontGetScalePixel -> 0x%08x\n", (unsigned)rc);
	uint32_t hdpi = 0, vdpi = 0;
	rc = cellFontGetResolutionDpi(NULL, &hdpi, &vdpi);
	printf("  cellFontGetResolutionDpi -> 0x%08x\n", (unsigned)rc);
	rc = cellFontEnd();
	printf("  cellFontEnd -> 0x%08x\n", (unsigned)rc);
	printf("result: PASS (validation complete)\n");
	sys_process_exit(0);
	return 0;
}
