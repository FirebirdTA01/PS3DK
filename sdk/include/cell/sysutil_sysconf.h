/*! \file cell/sysutil_sysconf.h
 \brief Sysconf utility-dialog launcher (cellSysconf*).

  Brings up one of the built-in system-configuration dialogs (audio
  device, voice changer, bluetooth manager, echo canceller).  Async;
  result is delivered via callback.
*/

#ifndef PS3TC_CELL_SYSUTIL_SYSCONF_H
#define PS3TC_CELL_SYSUTIL_SYSCONF_H

#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PS3TC_SYS_MEMORY_CONTAINER_T_DEFINED
#define PS3TC_SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

typedef enum {
    CELL_SYSCONF_TYPE_AUDIO_DEVICE_SETTINGS     = 0x00000001,
    CELL_SYSCONF_TYPE_VOICE_CHANGER             = 0x00000002,
    CELL_SYSCONF_TYPE_BLUETOOTH_DEVICE_UTILITY  = 0x00000003,
    CELL_SYSCONF_TYPE_ECHO_CANCELLER            = 0x00000004
} CellSysconfType;

typedef enum {
    CELL_SYSCONF_RESULT_OK        = 0,
    CELL_SYSCONF_RESULT_CANCELED  = 1
} CellSysconfResult;

#define CELL_SYSCONF_ERROR_PARAM   0x8002bb01

typedef void (*CellSysconfCallback)(int result, void *userdata);

extern int cellSysconfOpen(unsigned int type,
                           CellSysconfCallback func,
                           void *userdata,
                           void *extparam,
                           sys_memory_container_t id);
extern int cellSysconfAbort(void);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_SYSUTIL_SYSCONF_H */
