/* Compile-time ABI guard: library records are addressed by byte offset.
 * A typedef can require more alignment than its storage size. Use the
 * padded wrapper, not a bare job-chain array, for consecutive instances. */
#include <stddef.h>
#include <cell/spurs/job_chain_types.h>
#include <cell/spurs/workload.h>

#ifdef __cplusplus
#define CHECK(c, m) static_assert(c, m)
#else
#define CHECK(c, m) _Static_assert(c, m)
#endif
#define SIZE(t, n) CHECK(sizeof(t) == (n), #t " size")
#define ALIGN(t, n) CHECK(__alignof__(t) == (n), #t " alignment")
#define OFFSET(t, f, n) CHECK(offsetof(t, f) == (n), #t "." #f " offset")

SIZE(CellSpursJobChain, 272);
ALIGN(CellSpursJobChain, 128);
OFFSET(CellSpursJobChain, skip, 0);
SIZE(CellSpursJobChainForArray, 384);
ALIGN(CellSpursJobChainForArray, 128);
OFFSET(CellSpursJobChainForArray, jobChain, 0);
OFFSET(CellSpursJobChainForArray, padding, 272);
CHECK(sizeof(((CellSpursJobChainForArray *)0)->padding) == 112,
      "CellSpursJobChainForArray.padding width");
SIZE(CellSpursJobChainForArray[2], 768);
SIZE(CellSpursWorkloadFlag, 16);
ALIGN(CellSpursWorkloadFlag, 16);
OFFSET(CellSpursWorkloadFlag, flag, 12);
CHECK(sizeof(((CellSpursWorkloadFlag *)0)->flag) == 4,
      "CellSpursWorkloadFlag.flag width");

/* Emit real objects: alignment must hold for allocations and members. */
CellSpursJobChain spurs_layout_chain;
CellSpursJobChainForArray spurs_layout_array[2];
CellSpursWorkloadFlag spurs_layout_flag;
struct spurs_layout_embedding {
    char prefix;
    CellSpursJobChain chain;
    CellSpursWorkloadFlag flag;
};
OFFSET(struct spurs_layout_embedding, chain, 128);
OFFSET(struct spurs_layout_embedding, flag, 400);

unsigned int spurs_layout_read_flag(const CellSpursWorkloadFlag *p);
unsigned int spurs_layout_read_flag(const CellSpursWorkloadFlag *p)
{
    return p->flag;
}
