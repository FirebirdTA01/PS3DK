/*! \file cell/sysutil_print.h
 \brief Print to a USB / network printer through the system
        (libsysutil_print_stub).

  Load the print utility (LoadAsync with a memory container, LoadAsync2
  without), optionally let the user configure the printer, then print a job
  as pages sent in bands of R8G8B8 pixels: StartJob, then per page
  StartPage / SendBand ... / EndPage, then EndJob (or CancelJob), and
  unload.  GetPrintableArea gives the page size in pixels; GetStatus the
  printer state (CELL_PRINT_STATUS_*) and, on an error, the reason
  (CELL_PRINT_ERRORSTATUS_*).  Asynchronous steps call back with
  CELL_PRINT_RESULT_*.
*/

#ifndef PS3TC_CELL_SYSUTIL_PRINT_H
#define PS3TC_CELL_SYSUTIL_PRINT_H

#include <sys/memory.h>      /* sys_memory_container_t */
#include <sysutil/sysutil_common.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	CELL_PRINT_MODE_NORMAL = 0
};

enum {
	CELL_PRINT_PARAM_COLOR_FORMAT_R8G8B8 = 0
};

enum {
	CELL_PRINT_STATUS_IDLE       = 0,
	CELL_PRINT_STATUS_PRINTING   = 1,
	CELL_PRINT_STATUS_OFFLINE    = 2,
	CELL_PRINT_STATUS_BUSY       = 3,
	CELL_PRINT_STATUS_CANCELLING = 4,
	CELL_PRINT_STATUS_ERROR      = 5
};

/* errorStatus of a CELL_PRINT_STATUS_ERROR printer. */
#define CELL_PRINT_ERRORSTATUS_COMMON_OFFSET 0x80000000
#define CELL_PRINT_ERRORSTATUS_NONE                0x00000000
#define CELL_PRINT_ERRORSTATUS_COMM                0x80000000
#define CELL_PRINT_ERRORSTATUS_COVEROPEN           0x80000001
#define CELL_PRINT_ERRORSTATUS_INKCOVEROPEN        0x80000002
#define CELL_PRINT_ERRORSTATUS_NOPAPER             0x80000003
#define CELL_PRINT_ERRORSTATUS_NOTRAY              0x80000004
#define CELL_PRINT_ERRORSTATUS_SIZE_TYPE_PATH      0x80000005
#define CELL_PRINT_ERRORSTATUS_DOUBLEFEED          0x80000006
#define CELL_PRINT_ERRORSTATUS_FEEDERROR           0x80000007
#define CELL_PRINT_ERRORSTATUS_PAPERJAM            0x80000008
#define CELL_PRINT_ERRORSTATUS_TRAYCLOSE           0x80000009
#define CELL_PRINT_ERRORSTATUS_NOINK               0x8000000a
#define CELL_PRINT_ERRORSTATUS_NOCARTRIDGE         0x8000000b
#define CELL_PRINT_ERRORSTATUS_COVERFLOW           0x8000000c
#define CELL_PRINT_ERRORSTATUS_CFAIL               0x8000000d
#define CELL_PRINT_ERRORSTATUS_CFACTORY            0x8000000e
#define CELL_PRINT_ERRORSTATUS_BATTERYVOLTAGE      0x8000000f
#define CELL_PRINT_ERRORSTATUS_BATTERYTEMPERATURE  0x80000010
#define CELL_PRINT_ERRORSTATUS_BATTERYEMPTY        0x80000011
#define CELL_PRINT_ERRORSTATUS_SERVICEREQ          0x80000012
#define CELL_PRINT_ERRORSTATUS_GENERAL             0x80000013
#define CELL_PRINT_ERRORSTATUS_FATAL               0x80000014

enum {
	CELL_PRINT_RESULT_OK = 0,
	CELL_PRINT_RESULT_CANCEL,
	CELL_PRINT_RESULT_ERROR
};

#define CELL_PRINT_CONTINUE_DISABLED  0
#define CELL_PRINT_CONTINUE_ENABLED   1

typedef struct {
	unsigned int mode;
	unsigned char reserved[32];
} CellPrintLoadParam;

typedef struct {
	int status;
	int errorStatus;
	int continueEnabled;
	unsigned char reserved[32];
} CellPrintStatus;

#define CELL_PRINT_ERROR_INTERNAL             0x8002c401
#define CELL_PRINT_ERROR_NO_MEMORY            0x8002c402
#define CELL_PRINT_ERROR_PRINTER_NOT_FOUND    0x8002c403
#define CELL_PRINT_ERROR_INVALID_PARAM        0x8002c404
#define CELL_PRINT_ERROR_INVALID_FUNCTION     0x8002c405
#define CELL_PRINT_ERROR_NOT_SUPPORT          0x8002c406
#define CELL_PRINT_ERROR_OCCURRED             0x8002c407
#define CELL_PRINT_ERROR_CANCELED_BY_PRINTER  0x8002c408

typedef void (*CellPrintCallback)(int result, void *userdata);

int cellPrintLoadAsync(CellPrintCallback function, void *userdata,
                       const CellPrintLoadParam *param, sys_memory_container_t container);
int cellPrintLoadAsync2(CellPrintCallback function, void *userdata,
                        const CellPrintLoadParam *param);
int cellPrintUnloadAsync(CellPrintCallback function, void *userdata);
int cellPrintOpenConfig(CellPrintCallback function, void *userdata);
int cellPrintStartJob(int totalPage, int colorFormat);
int cellPrintEndJob(void);
int cellPrintCancelJob(void);
int cellPrintStartPage(void);
int cellPrintEndPage(void);
int cellPrintGetPrintableArea(int *pixelWidth, int *pixelHeight);
int cellPrintSendBand(const unsigned char *buff, int buffsize, int *sendsize);
int cellPrintGetStatus(CellPrintStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_PRINT_H */
