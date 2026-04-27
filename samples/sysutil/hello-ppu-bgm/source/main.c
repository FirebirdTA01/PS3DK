/*
 * hello-ppu-bgm — exercise the system-BGM playback control surface
 * (cellSysutil*BgmPlayback).  Flow:
 *
 *   1. Read current BGM status.
 *   2. Disable BGM (would silence XMB-selected music while in-game).
 *   3. Read status again to confirm transition to DISABLE.
 *   4. Re-enable, read status to confirm back to ENABLE.
 *   5. Exercise the *Ex variants with explicit fade-time params.
 *
 * Expected on RPCS3: rc=0 on every call, status transitions track
 * the requested state.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <cell/sysutil_bgmplayback.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const char *enable_label(uint8_t s)
{
	return s == CELL_SYSUTIL_BGMPLAYBACK_STATUS_ENABLE  ? "ENABLE"
	     : s == CELL_SYSUTIL_BGMPLAYBACK_STATUS_DISABLE ? "DISABLE"
	     : "?";
}

static const char *player_label(uint8_t s)
{
	return s == CELL_SYSUTIL_BGMPLAYBACK_STATUS_PLAY ? "PLAY"
	     : s == CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP ? "STOP"
	     : "?";
}

static void print_status(const char *tag)
{
	CellSysutilBgmPlaybackStatus st;
	memset(&st, 0, sizeof(st));
	int rc = cellSysutilGetBgmPlaybackStatus(&st);
	printf("  [%s] rc=0x%08x player=%s enable=%s fade=%u\n",
	       tag, (unsigned)rc,
	       player_label(st.playerState),
	       enable_label(st.enableState),
	       (unsigned)st.currentFadeRatio);
}

int main(void)
{
	printf("hello-ppu-bgm: cellSysutil*BgmPlayback exercise\n");

	print_status("initial");

	int rc = cellSysutilDisableBgmPlayback();
	printf("  cellSysutilDisableBgmPlayback -> 0x%08x\n", (unsigned)rc);
	usleep(100 * 1000);
	print_status("after-disable");

	rc = cellSysutilEnableBgmPlayback();
	printf("  cellSysutilEnableBgmPlayback -> 0x%08x\n", (unsigned)rc);
	usleep(100 * 1000);
	print_status("after-enable");

	CellSysutilBgmPlaybackExtraParam ex;
	memset(&ex, 0, sizeof(ex));
	ex.systemBgmFadeInTime  = 500;
	ex.systemBgmFadeOutTime = 500;
	ex.gameBgmFadeInTime    = 250;
	ex.gameBgmFadeOutTime   = 250;
	rc = cellSysutilSetBgmPlaybackExtraParam(&ex);
	printf("  cellSysutilSetBgmPlaybackExtraParam -> 0x%08x\n", (unsigned)rc);

	rc = cellSysutilDisableBgmPlaybackEx(&ex);
	printf("  cellSysutilDisableBgmPlaybackEx -> 0x%08x\n", (unsigned)rc);
	usleep(100 * 1000);

	rc = cellSysutilEnableBgmPlaybackEx(&ex);
	printf("  cellSysutilEnableBgmPlaybackEx -> 0x%08x\n", (unsigned)rc);

	CellSysutilBgmPlaybackStatus2 st2;
	memset(&st2, 0, sizeof(st2));
	rc = cellSysutilGetBgmPlaybackStatus2(&st2);
	printf("  cellSysutilGetBgmPlaybackStatus2 -> rc=0x%08x player=%s\n",
	       (unsigned)rc, player_label(st2.playerState));
	return 0;
}
