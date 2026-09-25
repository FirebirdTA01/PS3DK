/* SPDX-License-Identifier: BSD-2-Clause */
/* Default JQ adapter, isolated so a user's direct Main2 definition wins. */
#include <cell/spurs/job_context.h>
#include <cell/spurs/job_descriptor.h>

extern int _spurs_jq_syscall_initialize(CellSpursJobContext2 *, CellSpursJob256 *);
extern void _spurs_jq_syscall_finalize(CellSpursJobContext2 *);
extern void cellSpursJobQueueMain(CellSpursJobContext2 *, CellSpursJob256 *);

void cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    if (_spurs_jq_syscall_initialize(ctx, job) != 0)
        return;
    cellSpursJobQueueMain(ctx, job);
    _spurs_jq_syscall_finalize(ctx);
}
