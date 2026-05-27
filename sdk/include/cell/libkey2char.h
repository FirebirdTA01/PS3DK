/*
 * PS3 Custom Toolchain - cell/libkey2char.h
 *
 * cellKey2Char surface: keyboard-input to character-code conversion.
 * Five exported entry points backed by libcellKey2char_stub.a.
 */
#ifndef _PS3DK_CELL_LIBKEY2CHAR_H_
#define _PS3DK_CELL_LIBKEY2CHAR_H_

#include <stdint.h>
#include <cell/keyboard.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_KEY2CHAR_HANDLE_SIZE        128
#define CELL_K2C_MAKE_ERROR(status) \
	CELL_ERROR_MAKE_ERROR(CELL_ERROR_FACILITY_HID, status)

#define CELL_K2C_OK                         CELL_ERROR_CAST(0x00000000)
#define CELL_K2C_ERROR_FATAL                CELL_ERROR_CAST(0x80121301)
#define CELL_K2C_ERROR_INVALID_HANDLE       CELL_ERROR_CAST(0x80121302)
#define CELL_K2C_ERROR_INVALID_PARAMETER    CELL_ERROR_CAST(0x80121303)
#define CELL_K2C_ERROR_ALREADY_INITIALIZED  CELL_ERROR_CAST(0x80121304)
#define CELL_K2C_ERROR_UNINITIALIZED        CELL_ERROR_CAST(0x80121305)
#define CELL_K2C_ERROR_OTHER                CELL_ERROR_CAST(0x80121306)

typedef uint8_t CellKey2CharHandle[SCE_KEY2CHAR_HANDLE_SIZE];

typedef struct CellKey2CharKeyData {
	uint32_t led;
	uint32_t mkey;
	uint16_t keycode;
} CellKey2CharKeyData;

enum {
	CELL_KEY2CHAR_MODE_ENGLISH,
	CELL_KEY2CHAR_MODE_NATIVE,
	CELL_KEY2CHAR_MODE_NATIVE2
};

int32_t cellKey2CharOpen(CellKey2CharHandle handle);
int32_t cellKey2CharClose(CellKey2CharHandle handle);
int32_t cellKey2CharGetChar(CellKey2CharHandle handle,
                            CellKey2CharKeyData *kdata,
                            uint16_t **charCode,
                            uint32_t *charNum,
                            bool *processed);
int32_t cellKey2CharSetMode(CellKey2CharHandle handle, int32_t mode);
int32_t cellKey2CharSetArrangement(CellKey2CharHandle handle, uint32_t arrange);

#ifdef __cplusplus
}
#endif

#endif
