#ifndef __PS3DK_CELL_NP_CUSTOM_MENU_H__
#define __PS3DK_CELL_NP_CUSTOM_MENU_H__

#include <cell/np/common.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_CUSTOM_MENU_ACTION_CHARACTER_MAX 21
#define SCE_NP_CUSTOM_MENU_ACTION_ITEMS_MAX 7
#define SCE_NP_CUSTOM_MENU_ACTION_ITEMS_TOTAL_MAX 16
#define SCE_NP_CUSTOM_MENU_EXCEPTION_ITEMS_MAX 256

#define SCE_NP_CUSTOM_MENU_INDEX_SETSIZE 64
#define SCE_NP_CUSTOM_MENU_INDEX_BITS (sizeof(SceNpCustomMenuIndexMask) * 8)
#define SCE_NP_CUSTOM_MENU_INDEX_BITS_ALL ((SceNpCustomMenuIndexMask)-1)
#define SCE_NP_CUSTOM_MENU_INDEX_BITS_SHIFT 5
#define SCE_NP_CUSTOM_MENU_INDEX_BITS_MASK (SCE_NP_CUSTOM_MENU_INDEX_BITS - 1)
#define SCE_NP_CUSTOM_MENU_INDEX_BITS_MAX (SCE_NP_CUSTOM_MENU_INDEX_SETSIZE - 1)

#define SCE_NP_CUSTOM_MENU_INDEX_SET(n, p) ((p)->index_bits[(n) >> SCE_NP_CUSTOM_MENU_INDEX_BITS_SHIFT] |= (1U << ((n) & SCE_NP_CUSTOM_MENU_INDEX_BITS_MASK)))
#define SCE_NP_CUSTOM_MENU_INDEX_CLR(n, p) ((p)->index_bits[(n) >> SCE_NP_CUSTOM_MENU_INDEX_BITS_SHIFT] &= ~(1U << ((n) & SCE_NP_CUSTOM_MENU_INDEX_BITS_MASK)))
#define SCE_NP_CUSTOM_MENU_INDEX_ISSET(n, p) ((p)->index_bits[(n) >> SCE_NP_CUSTOM_MENU_INDEX_BITS_SHIFT] & (1U << ((n) & SCE_NP_CUSTOM_MENU_INDEX_BITS_MASK)))
#define SCE_NP_CUSTOM_MENU_INDEX_ZERO(p) do { \
    SceNpCustomMenuIndexArray *cell_np_custom_menu_array = (p); \
    uint32_t cell_np_custom_menu_i; \
    for (cell_np_custom_menu_i = 0; cell_np_custom_menu_i < (SCE_NP_CUSTOM_MENU_INDEX_SETSIZE >> SCE_NP_CUSTOM_MENU_INDEX_BITS_SHIFT); ++cell_np_custom_menu_i) \
        cell_np_custom_menu_array->index_bits[cell_np_custom_menu_i] = 0; \
} while (0)
#define SCE_NP_CUSTOM_MENU_INDEX_SET_ALL(p) do { \
    SceNpCustomMenuIndexArray *cell_np_custom_menu_array = (p); \
    uint32_t cell_np_custom_menu_i; \
    for (cell_np_custom_menu_i = 0; cell_np_custom_menu_i < (SCE_NP_CUSTOM_MENU_INDEX_SETSIZE >> SCE_NP_CUSTOM_MENU_INDEX_BITS_SHIFT); ++cell_np_custom_menu_i) \
        cell_np_custom_menu_array->index_bits[cell_np_custom_menu_i] = SCE_NP_CUSTOM_MENU_INDEX_BITS_ALL; \
} while (0)

#define SCE_NP_CUSTOM_MENU_ACTION_MASK_ME 0x00000001
#define SCE_NP_CUSTOM_MENU_ACTION_MASK_FRIEND 0x00000002
#define SCE_NP_CUSTOM_MENU_ACTION_MASK_PLAYER 0x00000004
#define SCE_NP_CUSTOM_MENU_ACTION_MASK_ALL \
    (SCE_NP_CUSTOM_MENU_ACTION_MASK_ME | SCE_NP_CUSTOM_MENU_ACTION_MASK_FRIEND | SCE_NP_CUSTOM_MENU_ACTION_MASK_PLAYER)

#define SCE_NP_CUSTOM_MENU_SELECTED_TYPE_ME 1
#define SCE_NP_CUSTOM_MENU_SELECTED_TYPE_FRIEND 2
#define SCE_NP_CUSTOM_MENU_SELECTED_TYPE_PLAYER 3

typedef uint32_t SceNpCustomMenuIndexMask;
typedef uint32_t SceNpCustomMenuActionMask;
typedef uint32_t SceNpCustomMenuSelectedType;

typedef struct SceNpCustomMenuIndexArray {
    SceNpCustomMenuIndexMask index_bits[SCE_NP_CUSTOM_MENU_INDEX_SETSIZE >> SCE_NP_CUSTOM_MENU_INDEX_BITS_SHIFT];
} SceNpCustomMenuIndexArray;

typedef struct SceNpCustomMenuAction {
    uint32_t options;
    const char *name ATTRIBUTE_PRXPTR;
    SceNpCustomMenuActionMask mask;
} SceNpCustomMenuAction;

typedef struct SceNpCustomMenu {
    uint64_t options;
    SceNpCustomMenuAction *actions ATTRIBUTE_PRXPTR;
    uint32_t numActions;
} SceNpCustomMenu;

typedef struct SceNpCustomMenuActionExceptions {
    uint32_t options;
    SceNpId npid;
    SceNpCustomMenuIndexArray actions;
    uint8_t reserved[4];
} SceNpCustomMenuActionExceptions;

typedef int (*SceNpCustomMenuEventHandler)(int retCode, uint32_t index, const SceNpId *npid, SceNpCustomMenuSelectedType type, void *arg);

int sceNpCustomMenuRegisterActions(const SceNpCustomMenu *menu, SceNpCustomMenuEventHandler handler, void *userArg, uint64_t options);
int sceNpCustomMenuActionSetActivation(const SceNpCustomMenuIndexArray *array, uint64_t options);
int sceNpCustomMenuRegisterExceptionList(const SceNpCustomMenuActionExceptions *items, size_t numItems, uint64_t options);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_CUSTOM_MENU_H__ */
