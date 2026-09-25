/* Canonical task entry bridge. Keep this in its own archive member so a
 * user's legacy cellSpursMain resolves the CRT without extracting it.
 * SPDX-License-Identifier: BSD-2-Clause */
#include <cell/spurs/task.h>

void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    cellSpursTaskExit(cellSpursTaskMain(argTask, argTaskset));
}
