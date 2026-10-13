/*! \file cell/sysutil_sysconf_ext.h
 \brief Paired Bluetooth devices (libsysutil_sysconf_ext_stub).

  cellSysconfBtGetDeviceList fills a list of up to 16 paired Bluetooth
  devices: an id, whether it is an audio or a HID device, whether it is
  available now, and its name.  The structures are written by the system,
  so their layout is fixed: 96 bytes per device, 1536 for the list.
*/

#ifndef PS3TC_CELL_SYSUTIL_SYSCONF_EXT_H
#define PS3TC_CELL_SYSUTIL_SYSCONF_EXT_H

#include <stdint.h>
#include <sysutil/sysutil_common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	CELL_SYSCONF_BT_DEVICE_TYPE_AUDIO = 1,
	CELL_SYSCONF_BT_DEVICE_TYPE_HID   = 2
} CellSysconfBtDeviceType;

typedef enum {
	CELL_SYSCONF_BT_DEVICE_STATE_UNAVAILABLE = 0,
	CELL_SYSCONF_BT_DEVICE_STATE_AVAILABLE   = 1
} CellSysconfBtDeviceState;

typedef struct CellSysconfBtDeviceInfo {
	uint64_t deviceId;
	CellSysconfBtDeviceType deviceType;
	CellSysconfBtDeviceState state;
	char name[64];
	uint32_t reserved[4];
} CellSysconfBtDeviceInfo;

typedef struct CellSysconfBtDeviceList {
	CellSysconfBtDeviceInfo device[16];
} CellSysconfBtDeviceList;

int cellSysconfBtGetDeviceList(CellSysconfBtDeviceList *deviceList);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_SYSCONF_EXT_H */
