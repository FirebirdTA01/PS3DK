#include "func_workload.h"
/* Keep the same five paths in optimized OFF and ON builds. The functions contain
 * no profiling API calls. Each leaf is a real call, including identical arguments
 * under different parents. noipa also prevents cross-function folding. */
__attribute__((noipa)) uint32_t sample_func_leaf(uint32_t x) { return 3u*x+7u; }
__attribute__((noipa)) uint32_t sample_func_left(uint32_t x)
{
    return sample_func_leaf(x)+sample_func_leaf(x+1u);
}
__attribute__((noipa)) uint32_t sample_func_right(uint32_t x)
{
    return sample_func_leaf(x)+sample_func_leaf(x+1u)+sample_func_leaf(x+2u);
}
__attribute__((noipa)) uint32_t sample_func_root(uint32_t x)
{
    return sample_func_left(x)+sample_func_right(x)+4u;
}
