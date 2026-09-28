/* spurs-suite LS patterns, SPU task.  argTask: u64[0] = result slot array
 * EA, u32[2] = report block EA, u32[3] = kind (ls_pattern.h). */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task.h>
#include "../ls_pattern.h"

#define TASK_INVAL  ((int)0x80410902u)
#define TASK_NOEXEC ((int)0x80410907u)
#define TASK_ALIGN  ((int)0x80410910u)
#define TASK_NULL   ((int)0x80410911u)

static result_slot out;
static lp_report report_block;
static unsigned char headers[0x300] __attribute__((aligned(16)));
static vec_uint4 record __attribute__((aligned(16)));

static void report(uint64_t slots, unsigned kind, unsigned step, unsigned extra)
{
    out.magic = RESULT_MAGIC | kind;
    out.status = step;
    out.value = cellSpursGetTaskId();
    out.extra = extra;
    mfc_put(&out, slots + kind * sizeof(result_slot), sizeof out, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

static int same(vec_uint4 a, vec_uint4 b)
{
    return spu_extract(spu_gather(spu_cmpeq(a, b)), 0) == 0xf;
}

static void store(unsigned int *to, vec_uint4 v)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        to[i] = spu_extract(v, i);
}

/* the pattern the task was launched with: every block above the SPURS area */
static const vec_uint4 k_all = { CELL_SPURS_TASK_TOP_MASK, 0xffffffffu, 0xffffffffu, 0xffffffffu };

#define EXPECT(step, cond, got) do { if (!(cond)) { report(slots, kind, (step), (unsigned)(got)); return; } } while (0)

static void context_steps(uint64_t slots, unsigned kind)
{
    vec_uint4 p, q;
    uint32_t size = 0;
    int rc;

    /* generate */
    EXPECT(1, (rc = cellSpursTaskGenerateLsPattern(0, 0x3000, 0x800)) == TASK_NULL, rc);
    EXPECT(2, (rc = cellSpursTaskGenerateLsPattern(&p, 0x3001, 0x800)) == TASK_ALIGN, rc);
    EXPECT(3, (rc = cellSpursTaskGenerateLsPattern(&p, 0x3000, 0x801)) == TASK_ALIGN, rc);
    EXPECT(4, (rc = cellSpursTaskGenerateLsPattern(&p, 0x3f800, 0x1000)) == TASK_INVAL, rc);
    EXPECT(5, (rc = cellSpursTaskGenerateLsPattern(&p, 0x3000, 0x40000 - 0x3000)) == 0, rc);
    EXPECT(6, same(p, k_all), spu_extract(p, 0));
    EXPECT(7, (rc = cellSpursTaskGenerateLsPattern(&p, 0x10000, 0x1800)) == 0, rc);
    q = (vec_uint4){ 0, 0xe0000000u, 0, 0 };                     /* blocks 32..34 */
    EXPECT(8, same(p, q), spu_extract(p, 1));
    EXPECT(9, same(cellSpursContextGenerateLsPattern(0x10000, 0x1800), q), 0);

    /* save-area size: 2 KB per block plus the execution context */
    EXPECT(10, (rc = cellSpursTaskGetContextSaveAreaSize(&size, k_all)) == 0, rc);
    EXPECT(11, size == 122 * 2048 + 1024, size);
    EXPECT(12, (rc = cellSpursTaskGetContextSaveAreaSize(&size, (vec_uint4){ 0x80000000u, 0, 0, 0 })) == TASK_INVAL, rc);

    /* the running task's pattern */
    p = cellSpursContextGetLsPattern();
    EXPECT(13, same(p, k_all), spu_extract(p, 0));
    EXPECT(14, (rc = cellSpursContextSetLsPattern((vec_uint4){ 0x04000000u, 0, 0, 0 } | k_all)) == TASK_INVAL, rc);
    EXPECT(15, (rc = cellSpursContextSetLsPattern(q)) == 0, rc);
    p = cellSpursContextGetLsPattern();
    EXPECT(16, same(p, q), spu_extract(p, 1));

    /* ... and the task's record in the taskset follows it */
    mfc_write_tag_mask(0xffffffffu);
    mfc_read_tag_status_all();
    mfc_get(&record, cellSpursGetTasksetAddress() + 0x80 + cellSpursGetTaskId() * 0x30 + 0x20, 16, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    EXPECT(17, same(record, q), spu_extract(record, 1));

    /* a yield keeps it */
    EXPECT(18, (rc = cellSpursYield()) == 0, rc);
    p = cellSpursContextGetLsPattern();
    EXPECT(19, same(p, q), spu_extract(p, 1));

    /* back to the whole task area */
    EXPECT(20, (rc = cellSpursContextSetLsPattern(k_all)) == 0, rc);
    report(slots, kind, 0, 0);
}

static void elf_steps(uint64_t slots, unsigned kind, uint64_t reportEa)
{
    const uint64_t elf = cellSpursGetElfAddress();
    vec_uint4 p;
    int rc;

    EXPECT(1, (rc = cellSpursTaskGetLoadableSegmentPattern(0, elf, 0, 2)) == TASK_NULL, rc);
    EXPECT(2, (rc = cellSpursTaskGetLoadableSegmentPattern(&p, elf, 0, 32)) == TASK_INVAL, rc);
    EXPECT(3, (rc = cellSpursTaskGetLoadableSegmentPattern(&p, 0, 0, 2)) == TASK_NULL, rc);
    EXPECT(4, (rc = cellSpursTaskGetReadOnlyAreaPattern(&p, elf + 4, 0, 2)) == TASK_ALIGN, rc);
    EXPECT(5, (rc = cellSpursTaskGetReadOnlyAreaPattern(&p, elf, headers + 4, 2)) == TASK_ALIGN, rc);
    /* the result slots are not an ELF */
    EXPECT(6, (rc = cellSpursTaskGetReadOnlyAreaPattern(&p, slots, 0, 2)) == TASK_NOEXEC, rc);

    EXPECT(7, (rc = cellSpursTaskGetLoadableSegmentPattern(&p, elf, 0, 2)) == 0, rc);
    store(report_block.loadable, p);
    EXPECT(8, (rc = cellSpursTaskGetReadOnlyAreaPattern(&p, elf, 0, 2)) == 0, rc);
    store(report_block.readonly, p);
    EXPECT(9, (rc = cellSpursTaskGetLoadableSegmentPattern(&p, elf, headers, 3)) == 0, rc);
    store(report_block.loadable_buf, p);
    EXPECT(10, (rc = cellSpursTaskGetReadOnlyAreaPattern(&p, elf, headers, 3)) == 0, rc);
    store(report_block.readonly_buf, p);
    /* the caller buffer holds the ELF header */
    EXPECT(11, headers[0] == 0x7f && headers[1] == 'E' && headers[0x13] == 23, headers[0x13]);

    mfc_put(&report_block, reportEa, sizeof report_block, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    report(slots, kind, 0, 0);
}

void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    struct { uint64_t slots; uint32_t reportEa, kind; } arg __attribute__((aligned(16)));
    (void)argTaskset;
    *(qword *)&arg = argTask;
    if (arg.kind == LP_CONTEXT)
        context_steps(arg.slots, arg.kind);
    else
        elf_steps(arg.slots, arg.kind, arg.reportEa);
}
