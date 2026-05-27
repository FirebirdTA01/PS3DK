/*
 * hello-ppu-imejp - cellImeJpUtility surface validation.
 *
 * Exercises all 41 entry points in libsysutil_imejp_stub.a.
 * Open returns OK; subsequent calls use that handle.  Expected
 * return codes under RPCS3 HLE:
 *   Open:        0x00000000  (CELL_OK - HLE stub returns OK but does
 *                             not fully initialize the handle)
 *   Open2/Open3: 0x8002BF21  (CELL_IMEJP_ERROR_ALREADY_OPEN)
 *   Set*Mode:    0x00000000  (CELL_OK - handle partially init'd)
 *   GetStatus:   0x00000000  (CELL_OK)
 *   EnterChar*:  0x00000000  (CELL_OK)
 *   ModeCaret*:  0x8002BF01  (CELL_IMEJP_ERROR_ERR)
 *   Convert/Confirm: 0x8002BF01 (ERR)
 *   GetFocus*:   0x00000000  (CELL_OK)
 *   GetCandidateList: 0x8002BF01 (ERR)
 *   GetConfirmString: 0x00000000 (CELL_OK)
 *   Close:       0x00000000  (CELL_OK)
 * cellSysutilRegisterCallback(NULL): tolerated, expected 0x00000000.
 * The sample links, calls every stub, prints each return code, and
 * reaches sys_process_exit(0) regardless of runtime results.
 *
 * All calls use an INVALID handle (or NULL) since RPCS3 does not
 * HLE the IME-JP dialogs.  Return codes will be error values;
 * the validation gate is binary boot + all-41-called + clean exit.
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/process.h>
#include <sys/memory.h>

#include <cell/sysutil.h>
#include <sysutil/sysutil_imejp.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-imejp: cellImeJpUtility surface validation\n");

	int rc = cellSysutilRegisterCallback(0, NULL, NULL);
	printf("  cellSysutilRegisterCallback -> 0x%08x\n", (unsigned)rc);

	CellImeJpHandle handle = {0};
	int16_t out_status = 0;
	sys_memory_container_t cid =
		(sys_memory_container_t)SYS_MEMORY_CONTAINER_ID_INVALID;

	/* Open family */
	rc = cellImeJpOpen(cid, &handle, NULL);
	printf("  cellImeJpOpen -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpOpen2(cid, &handle, NULL);
	printf("  cellImeJpOpen2 -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpOpen3(cid, &handle, NULL);
	printf("  cellImeJpOpen3 -> 0x%08x\n", (unsigned)rc);

	/* Mode settings */
	rc = cellImeJpSetKanaInputMode(handle, 0);
	printf("  cellImeJpSetKanaInputMode -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpSetInputCharType(handle, 0);
	printf("  cellImeJpSetInputCharType -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpSetFixInputMode(handle, 0);
	printf("  cellImeJpSetFixInputMode -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpAllowExtensionCharacters(handle, 0);
	printf("  cellImeJpAllowExtensionCharacters -> 0x%08x\n", (unsigned)rc);

	/* Reset / status */
	rc = cellImeJpReset(handle);
	printf("  cellImeJpReset -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpGetStatus(handle, &out_status);
	printf("  cellImeJpGetStatus -> 0x%08x, status=%d\n",
	       (unsigned)rc, (int)out_status);

	/* Character / string entry */
	rc = cellImeJpEnterChar(handle, 0, &out_status);
	printf("  cellImeJpEnterChar -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpEnterCharExt(handle, 0, &out_status);
	printf("  cellImeJpEnterCharExt -> 0x%08x\n", (unsigned)rc);
	uint16_t str[2] = {0,0};
	rc = cellImeJpEnterString(handle, str, &out_status);
	printf("  cellImeJpEnterString -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpEnterStringExt(handle, str, &out_status);
	printf("  cellImeJpEnterStringExt -> 0x%08x\n", (unsigned)rc);

	/* Cursor movement */
	rc = cellImeJpModeCaretRight(handle);
	printf("  cellImeJpModeCaretRight -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpModeCaretLeft(handle);
	printf("  cellImeJpModeCaretLeft -> 0x%08x\n", (unsigned)rc);

	/* Word operations */
	rc = cellImeJpBackspaceWord(handle);
	printf("  cellImeJpBackspaceWord -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpDeleteWord(handle);
	printf("  cellImeJpDeleteWord -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpAllDeleteConvertString(handle);
	printf("  cellImeJpAllDeleteConvertString -> 0x%08x\n", (unsigned)rc);

	/* Convert operations */
	rc = cellImeJpConvertForward(handle);
	printf("  cellImeJpConvertForward -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpConvertBackward(handle);
	printf("  cellImeJpConvertBackward -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpConvertCancel(handle);
	printf("  cellImeJpConvertCancel -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpAllConvertCancel(handle);
	printf("  cellImeJpAllConvertCancel -> 0x%08x\n", (unsigned)rc);

	/* Confirmations */
	rc = cellImeJpAllConfirm(handle);
	printf("  cellImeJpAllConfirm -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpCurrentPartConfirm(handle, 0);
	printf("  cellImeJpCurrentPartConfirm -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpTemporalConfirm(handle, 0);
	printf("  cellImeJpTemporalConfirm -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpConfirmPrediction(handle, NULL);
	printf("  cellImeJpConfirmPrediction -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpPostConvert(handle, 0);
	printf("  cellImeJpPostConvert -> 0x%08x\n", (unsigned)rc);

	/* Area manipulation */
	rc = cellImeJpExtendConvertArea(handle);
	printf("  cellImeJpExtendConvertArea -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpShortenConvertArea(handle);
	printf("  cellImeJpShortenConvertArea -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpMoveFocusClause(handle, 0);
	printf("  cellImeJpMoveFocusClause -> 0x%08x\n", (unsigned)rc);

	/* Focus queries */
	int16_t out_val = 0;
	rc = cellImeJpGetFocusTop(handle, &out_val);
	printf("  cellImeJpGetFocusTop -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpGetFocusLength(handle, &out_val);
	printf("  cellImeJpGetFocusLength -> 0x%08x\n", (unsigned)rc);

	/* Candidate / prediction lists */
	uint16_t cbuf[32] = {0};
	int16_t listNum = 32;
	rc = cellImeJpGetCandidateList(handle, &listNum, cbuf);
	printf("  cellImeJpGetCandidateList -> 0x%08x\n", (unsigned)rc);
	int16_t csize = 0;
	rc = cellImeJpGetCandidateListSize(handle, &csize);
	printf("  cellImeJpGetCandidateListSize -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpGetCandidateSelect(handle, &out_val);
	printf("  cellImeJpGetCandidateSelect -> 0x%08x\n", (unsigned)rc);

	/* String outputs */
	uint16_t obuf[256];
	rc = cellImeJpGetConfirmString(handle, obuf);
	printf("  cellImeJpGetConfirmString -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpGetConfirmYomiString(handle, obuf);
	printf("  cellImeJpGetConfirmYomiString -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpGetConvertString(handle, obuf);
	printf("  cellImeJpGetConvertString -> 0x%08x\n", (unsigned)rc);
	rc = cellImeJpGetConvertYomiString(handle, obuf);
	printf("  cellImeJpGetConvertYomiString -> 0x%08x\n", (unsigned)rc);
	CellImeJpPredictItem pitems[8];
	int32_t plistCount = 0;
	rc = cellImeJpGetPredictList(handle, obuf, 8, &plistCount, pitems);
	printf("  cellImeJpGetPredictList -> 0x%08x\n", (unsigned)rc);

	/* Close */
	rc = cellImeJpClose(handle);
	printf("  cellImeJpClose -> 0x%08x\n", (unsigned)rc);

	cellSysutilUnregisterCallback(0);
	printf("result: PASS (validation complete)\n");

	sys_process_exit(0);
	return 0;
}
