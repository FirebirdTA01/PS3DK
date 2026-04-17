/*
 * hello-ppu-l10n — port of Sony's samples/sdk/l10n/eucjp2sjis.c.
 *
 * Validates the full Phase 6.5 stack end-to-end on a *real* Sony sample:
 *   - Sony-named cellSysmoduleLoadModule / UnloadModule via
 *     <cell/sysmodule.h>, backed by libsysmodule_stub.a.
 *   - Sony-named L10N converter eucjp2sjis() via <cell/l10n.h>,
 *     backed by libl10n_stub.a (165 exports — first nidgen-archive
 *     consumer above ~10 exports).
 *
 * Differences from Sony's eucjp2sjis.c:
 *   - No argv parsing (RPCS3 launches don't pass args reliably);
 *     hardcode a few EUC-JP code points instead.
 *   - Diagnostics adjusted for our printf style; logic identical.
 *
 * Imports Sony FNIDs:
 *   cellSysmoduleLoadModule    0x32267a31
 *   cellSysmoduleUnloadModule  0x112a5ee9
 *   eucjp2sjis                 (computed at runtime; FNID lives in the .a)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/process.h>

#include <cell/sysmodule.h>
#include <cell/l10n.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-l10n: port of Sony eucjp2sjis sample\n");

	int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_L10N);
	printf("  cellSysmoduleLoadModule(L10N) -> 0x%08x\n", (unsigned)rc);
	if (rc != CELL_SYSMODULE_LOADED) {
		printf("  load failed; bailing\n");
		return 1;
	}

	/* A few EUC-JP code points → SJIS expected outputs.
	 * (Hardcoded since RPCS3 doesn't usually pass argv.) */
	static const uint16_t inputs[] = {
		0xa4a2,  /* HIRAGANA "a" — expect 0x82a0 */
		0xa4a4,  /* HIRAGANA "i" — expect 0x82a2 */
		0xc6fc,  /* KANJI "nichi" — expect 0x93fa */
		0xcbdc,  /* KANJI "hon" — expect 0x967b */
	};
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
		uint16_t sjis = eucjp2sjis(inputs[i]);
		printf("  eucjp2sjis(0x%04x) -> 0x%04x\n", inputs[i], sjis);
	}

	rc = cellSysmoduleUnloadModule(CELL_SYSMODULE_L10N);
	printf("  cellSysmoduleUnloadModule(L10N) -> 0x%08x\n", (unsigned)rc);

	printf("hello-ppu-l10n: done\n");
	return 0;
}
