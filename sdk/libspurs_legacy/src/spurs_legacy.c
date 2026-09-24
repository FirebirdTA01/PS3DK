/* sdk/libspurs_legacy/src/spurs_legacy.c
 *
 * PSL1GHT's spursAttributeInitialize, which is not a firmware export.
 *
 * Every other PSL1GHT spurs* name is a nidgen alias of the reference
 * export it called (libspurs_stub.yaml). This one was C in PSL1GHT: it
 * supplied the attribute revision and internal SDK version and called
 * _cellSpursAttributeInitialize. That is exactly what the SDK's inline
 * cellSpursAttributeInitialize does, so the compatibility entry point
 * forwards to it rather than repeating the constants.
 */

#include <stdbool.h>
#include <stdint.h>

#include <cell/spurs/types.h>

int32_t spursAttributeInitialize(CellSpursAttribute *attr, uint8_t nSpus,
                                 int32_t spuPriority, int32_t ppuPriority,
                                 bool exitIfNoWork);

int32_t spursAttributeInitialize(CellSpursAttribute *attr, uint8_t nSpus,
                                 int32_t spuPriority, int32_t ppuPriority,
                                 bool exitIfNoWork)
{
    return cellSpursAttributeInitialize(attr, nSpus, spuPriority, ppuPriority,
                                        exitIfNoWork);
}
