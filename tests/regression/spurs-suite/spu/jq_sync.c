/* spurs-jq-sync row, SPU job (-mspurs-job-initialize).  A ROW job works for
 * a while, then writes its row slot.  A COLUMN job reads every row slot and
 * reports how many rows were finished when it ran.
 * workArea.userData: [0] rows EA, [1] columns EA, [2] kind, [3] index. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_context.h>
#include "../jq_sync.h"

static uint32_t s_rows[JS_ROWS * 4] __attribute__((aligned(128)));

void cellSpursJobQueueMain(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    __attribute__((aligned(16))) uint32_t buf[4] = { 0, 0, 0, 0 };
    (void)ctx;
    if (!job)
        return;
    uint64_t rowsEa = job->workArea.userData[0], colsEa = job->workArea.userData[1];
    uint32_t kind = (uint32_t)job->workArea.userData[2], index = (uint32_t)job->workArea.userData[3];

    if (kind == JS_ROW) {
        for (volatile uint32_t spin = 0; spin < JS_SPIN; ++spin) {
        }
        buf[0] = JS_ROW_DONE | index;
        mfc_put(buf, rowsEa + index * 16, sizeof buf, 0, 0, 0);
    } else {
        mfc_get(s_rows, rowsEa, sizeof s_rows, 0, 0, 0);
        mfc_write_tag_mask(1u << 0);
        mfc_read_tag_status_all();
        uint32_t seen = 0;
        for (uint32_t r = 0; r < JS_ROWS; ++r)
            seen += s_rows[r * 4] == (JS_ROW_DONE | r);
        buf[0] = JS_COL_DONE | index;
        buf[1] = seen;
        mfc_put(buf, colsEa + index * 16, sizeof buf, 0, 0, 0);
    }
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();
}
