/*
 * hello-ppu-backfill — Phase 6.5 batch link smoke test.
 *
 * Includes the five new Phase 6.5 sysutil headers (subdisplay, music,
 * music_decode, music_export, imejp) and references one function from
 * each library so the linker pulls in the corresponding stub archive
 * and writes its FNID + sceResident entries into the ELF.
 *
 * The references are *taken* (function pointers stored in a volatile
 * sink) but never *called*.  This proves linkage end-to-end without
 * needing each library's full runtime to be LLE'd in RPCS3 — calling
 * cellMusicInitialize (etc.) on a non-LLE module crashes; taking the
 * pointer just touches the .opd descriptor.
 *
 * For the runtime check, just confirm the print line appears.  The
 * SPRX modules don't need to load — only the link-time machinery does.
 */

#include <stdint.h>
#include <stdio.h>

#include <sys/process.h>

#include <cell/sysutil_subdisplay.h>
#include <cell/sysutil_music.h>
#include <cell/sysutil_music_decode.h>
#include <cell/sysutil_music_export.h>
#include <cell/sysutil_imejp.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static volatile const void *g_link_anchors[] = {
	(const void *)cellSubDisplayInit,
	(const void *)cellSubDisplayEnd,
	(const void *)cellMusicInitialize,
	(const void *)cellMusicFinalize,
	(const void *)cellMusicDecodeInitialize,
	(const void *)cellMusicDecodeFinalize,
	(const void *)cellMusicExportInitialize,
	(const void *)cellMusicExportFinalize,
	(const void *)cellImeJpOpen,
	(const void *)cellImeJpClose,
};

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	const size_t n = sizeof(g_link_anchors) / sizeof(g_link_anchors[0]);
	printf("hello-ppu-backfill: %zu Phase 6.5 stub-archive symbols anchored\n", n);
	printf("  subdisplay  : Init=%p, End=%p\n",
	       g_link_anchors[0], g_link_anchors[1]);
	printf("  music       : Initialize=%p, Finalize=%p\n",
	       g_link_anchors[2], g_link_anchors[3]);
	printf("  music_decode: Initialize=%p, Finalize=%p\n",
	       g_link_anchors[4], g_link_anchors[5]);
	printf("  music_export: Initialize=%p, Finalize=%p\n",
	       g_link_anchors[6], g_link_anchors[7]);
	printf("  imejp       : Open=%p, Close=%p\n",
	       g_link_anchors[8], g_link_anchors[9]);
	printf("hello-ppu-backfill: done\n");
	return 0;
}
