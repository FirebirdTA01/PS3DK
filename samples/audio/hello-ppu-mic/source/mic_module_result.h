#pragma once
#include <cell/sysmodule.h>
namespace mic_sample {
// Session acquisition uses 0 for owned and 1 for an already loaded module.
constexpr int module_result(int rc) {
    return uint32_t(rc) == CELL_SYSMODULE_ERROR_DUPLICATED ? 1 : rc;
}
}
