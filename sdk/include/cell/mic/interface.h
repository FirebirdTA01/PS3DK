#ifndef __PS3DK_CELL_MIC_INTERFACE_H__
#define __PS3DK_CELL_MIC_INTERFACE_H__

#include <stdint.h>
#include <sys/event.h>
#include <cell/mic/define.h>
#include <cell/mic/error.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellMicClose(int dev_num);
int cellMicEnd(void);
int cellMicGetDeviceAttr(int dev_num, CellMicDeviceAttr dev_attrib,
                         int *arg1, int *arg2);
int cellMicGetDeviceGUID(int dev_num, uint32_t *ptr_guid);
int cellMicGetFormat(int dev_num, CellMicInputFormatI *format);
int cellMicGetFormatAux(int dev_num, CellMicInputFormatI *format);
int cellMicGetFormatDsp(int dev_num, CellMicInputFormatI *format);
int cellMicGetFormatRaw(int dev_num, CellMicInputFormatI *format);
int cellMicGetSignalAttr(int dev_num, CellMicSignalAttr sig_attrib, void *value);
int cellMicGetSignalState(int dev_num, CellMicSignalState sig_state, void *value);
int cellMicGetStatus(int dev_num, CellMicStatus *status);
int cellMicGetType(int dev_num, int *ptr_type);
int cellMicInit(void);
int cellMicIsAttached(int dev_num);
int cellMicIsOpen(int dev_num);
int cellMicOpen(int dev_num, int samprate);
int cellMicOpenEx(int dev_num, int raw_samprate, int raw_channel,
                  int dsp_samprate, int bufms, CellMicSignalType sigtype);
int cellMicOpenRaw(int dev_num, int samprate, int channel);
int cellMicRead(int dev_num, void *data, int max_bytes);
int cellMicReadAux(int dev_num, void *data, int max_bytes);
int cellMicReadDsp(int dev_num, void *data, int max_bytes);
int cellMicReadRaw(int dev_num, void *data, int max_bytes);
int cellMicRemoveNotifyEventQueue(sys_ipc_key_t key);
int cellMicReset(int dev_num);
int cellMicSetDeviceAttr(int dev_num, CellMicDeviceAttr dev_attrib,
                         int arg1, int arg2);
int cellMicSetNotifyEventQueue(sys_ipc_key_t key);
int cellMicSetNotifyEventQueue2(sys_ipc_key_t key, uint64_t source, uint64_t flag);
int cellMicSetSignalAttr(int dev_num, CellMicSignalAttr sig_attrib, void *value);
int cellMicStart(int dev_num);
int cellMicStartEx(int dev_num, uint32_t iFlags);
int cellMicStop(int dev_num);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_MIC_INTERFACE_H__ */
