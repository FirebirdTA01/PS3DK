/*! \file cell/sysutil_imejp.h
 \brief Sony-SDK-source-compat libsysutil_imejp (Japanese IME) API.

  Backed by libsysutil_imejp_stub.a — 41 exports against the cellSysutilMisc
  SPRX module.  No PSL1GHT runtime wrapper.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_IMEJP_H__
#define __PSL1GHT_CELL_SYSUTIL_IMEJP_H__

#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque IME handle. */
typedef void *CellImeJpHandle;

/* Optional dictionary path. */
#define CELL_IMEJP_DIC_PATH_MAXLENGTH    256
typedef int8_t CellImeJpAddDic[CELL_IMEJP_DIC_PATH_MAXLENGTH];

/* Error codes. */
#define CELL_IMEJP_ERROR_ERR                  0x8002bf01
#define CELL_IMEJP_ERROR_CONTEXT              0x8002bf11
#define CELL_IMEJP_ERROR_ALREADY_OPEN         0x8002bf21
#define CELL_IMEJP_ERROR_DIC_OPEN             0x8002bf31
#define CELL_IMEJP_ERROR_PARAM                0x8002bf41
#define CELL_IMEJP_ERROR_IME_ALREADY_IN_USE   0x8002bf51
#define CELL_IMEJP_ERROR_OTHER                0x8002bfff

/* Kana input mode (cellImeJpSetKanaInputMode). */
#define CELL_IMEJP_ROMAN_INPUT          0
#define CELL_IMEJP_KANA_INPUT           1

/* Display character type (cellImeJpSetInputCharType). */
#define CELL_IMEJP_DSPCHAR_HIRA         1
#define CELL_IMEJP_DSPCHAR_FKANA        2
#define CELL_IMEJP_DSPCHAR_RAW          3
#define CELL_IMEJP_DSPCHAR_HKANA        4
#define CELL_IMEJP_DSPCHAR_HRAW         5

/* Fixed input mode (cellImeJpSetFixInputMode). */
#define CELL_IMEJP_FIXINMODE_OFF        0
#define CELL_IMEJP_FIXINMODE_HIRA       1
#define CELL_IMEJP_FIXINMODE_FKANA      2
#define CELL_IMEJP_FIXINMODE_RAW        3
#define CELL_IMEJP_FIXINMODE_HKANA      4
#define CELL_IMEJP_FIXINMODE_HRAW       5

/* Extension chars (cellImeJpAllowExtensionCharacters). */
#define CELL_IMEJP_EXTENSIONCH_NONE      0x0000
#define CELL_IMEJP_EXTENSIONCH_HANKANA   0x0001
#define CELL_IMEJP_EXTENSIONCH_UD09TO15  0x0004
#define CELL_IMEJP_EXTENSIONCH_UD85TO94  0x0008
#define CELL_IMEJP_EXTENSIONCH_OUTJIS    0x0010

/* IME status (cellImeJpGetStatus). */
#define CELL_IMEJP_BEFORE_INPUT          0
#define CELL_IMEJP_BEFORE_CONVERT        1
#define CELL_IMEJP_CONVERTING            2
#define CELL_IMEJP_CANDIDATE_EMPTY       3
#define CELL_IMEJP_POSTCONVERT_KANA      4
#define CELL_IMEJP_POSTCONVERT_HALF      5
#define CELL_IMEJP_POSTCONVERT_RAW       6
#define CELL_IMEJP_CANDIDATES            7
#define CELL_IMEJP_MOVE_CLAUSE_GAP       8

/* Post-conversion (cellImeJpPostConvert). */
#define CELL_IMEJP_POSTCONV_HIRA         1
#define CELL_IMEJP_POSTCONV_KANA         2
#define CELL_IMEJP_POSTCONV_HALF         3
#define CELL_IMEJP_POSTCONV_RAW          4

/* Focus movement (cellImeJpMoveFocusClause). */
#define CELL_IMEJP_FOCUS_NEXT            0
#define CELL_IMEJP_FOCUS_BEFORE          1
#define CELL_IMEJP_FOCUS_TOP             2
#define CELL_IMEJP_FOCUS_END             3

/* Edit/convert result codes. */
#define CELL_IMEJP_RET_NONE              0
#define CELL_IMEJP_RET_THROUGH           1
#define CELL_IMEJP_RET_CONFIRMED         2

/* sys_memory_container_t bridge. */
#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

typedef struct CellImeJpPredictItem {
	uint16_t KanaYomi[31];
	uint16_t Hyoki[61];
} CellImeJpPredictItem;

/* Open / close. */
int cellImeJpOpen (sys_memory_container_t container_id, CellImeJpHandle *hImeJpHandle, const CellImeJpAddDic addDicPath);
int cellImeJpOpen2(sys_memory_container_t container_id, CellImeJpHandle *hImeJpHandle, const CellImeJpAddDic addDicPath);
int cellImeJpOpen3(sys_memory_container_t container_id, CellImeJpHandle *hImeJpHandle, const CellImeJpAddDic *addDicPath);
int cellImeJpClose(CellImeJpHandle hImeJpHandle);

/* Configuration. */
int cellImeJpSetKanaInputMode(CellImeJpHandle hImeJpHandle, int16_t inputOption);
int cellImeJpSetInputCharType(CellImeJpHandle hImeJpHandle, int16_t charTypeOption);
int cellImeJpSetFixInputMode (CellImeJpHandle hImeJpHandle, int16_t fixInputMode);
int cellImeJpAllowExtensionCharacters(CellImeJpHandle hImeJpHandle, int16_t extentionCharacters);
int cellImeJpReset(CellImeJpHandle hImeJpHandle);

/* Status. */
int cellImeJpGetStatus(CellImeJpHandle hImeJpHandle, int16_t *pInputStatus);

/* Edit / convert. */
int cellImeJpEnterChar           (CellImeJpHandle h, uint16_t inputChar, int16_t *pOutputStatus);
int cellImeJpEnterCharExt        (CellImeJpHandle h, uint16_t inputChar, int16_t *pOutputStatus);
int cellImeJpEnterString         (CellImeJpHandle h, const uint16_t *pInputString, int16_t *pOutputStatus);
int cellImeJpEnterStringExt      (CellImeJpHandle h, const uint16_t *pInputString, int16_t *pOutputStatus);
int cellImeJpModeCaretRight      (CellImeJpHandle h);
int cellImeJpModeCaretLeft       (CellImeJpHandle h);
int cellImeJpBackspaceWord       (CellImeJpHandle h);
int cellImeJpDeleteWord          (CellImeJpHandle h);
int cellImeJpAllDeleteConvertString(CellImeJpHandle h);
int cellImeJpConvertForward      (CellImeJpHandle h);
int cellImeJpConvertBackward     (CellImeJpHandle h);
int cellImeJpCurrentPartConfirm  (CellImeJpHandle h, int16_t listItem);
int cellImeJpAllConfirm          (CellImeJpHandle h);
int cellImeJpConvertCancel       (CellImeJpHandle h);
int cellImeJpAllConvertCancel    (CellImeJpHandle h);
int cellImeJpExtendConvertArea   (CellImeJpHandle h);
int cellImeJpShortenConvertArea  (CellImeJpHandle h);

/* Candidate list. */
int cellImeJpGetCandidateListSize(CellImeJpHandle h, int16_t *pListSize);
int cellImeJpGetCandidateList    (CellImeJpHandle h, int16_t *plistNum, uint16_t *pCandidateString);
int cellImeJpTemporalConfirm     (CellImeJpHandle h, int16_t selectIndex);
int cellImeJpPostConvert         (CellImeJpHandle h, int16_t postType);
int cellImeJpMoveFocusClause     (CellImeJpHandle h, int16_t moveType);
int cellImeJpGetFocusTop         (CellImeJpHandle h, int16_t *pFocusTop);
int cellImeJpGetFocusLength      (CellImeJpHandle h, int16_t *pFocusLength);
int cellImeJpGetConfirmYomiString(CellImeJpHandle h, uint16_t *pYomiString);
int cellImeJpGetConfirmString    (CellImeJpHandle h, uint16_t *pConfirmString);
int cellImeJpGetConvertYomiString(CellImeJpHandle h, uint16_t *pYomiString);
int cellImeJpGetConvertString    (CellImeJpHandle h, uint16_t *pConvertString);
int cellImeJpGetCandidateSelect  (CellImeJpHandle h, int16_t *pIndex);

/* Prediction. */
int cellImeJpGetPredictList      (CellImeJpHandle h, uint16_t *pYomiString,
                                  int32_t itemNum, int32_t *plistCount,
                                  CellImeJpPredictItem *pPredictItem);
int cellImeJpConfirmPrediction   (CellImeJpHandle h, CellImeJpPredictItem *pPredictItem);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_IMEJP_H__ */
