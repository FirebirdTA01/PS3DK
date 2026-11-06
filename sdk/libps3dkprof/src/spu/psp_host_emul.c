/* HOST EMULATION of the SPU lane premises: see psp_host_emul.h.  Host builds only.
 * One ownership mapping: a line's mutex and its version counter are the SAME stripe, so every version access is
 * protected by the one mutex that owns it.  Lines sharing a stripe share its version (a conservative reservation
 * loss), never a counter under two different locks. */
#include "psp_host_emul.h"

#include <pthread.h>

#define STRIPES 256u

static pthread_mutex_t g_line[STRIPES];
static uint64_t g_ver[STRIPES]; /* g_ver[i] is owned by g_line[i] */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
int psp_emul_drop_line_a;

static void init(void)
{
    for (unsigned i = 0; i < STRIPES; i++)
        pthread_mutex_init(&g_line[i], NULL);
}

unsigned psp_emul_stripe(uint64_t ea) { return (unsigned)((ea >> 7) * 0x9E3779B1u >> 24) % STRIPES; }

void psp_emul_line_lock(uint64_t ea)
{
    pthread_once(&g_once, init);
    pthread_mutex_lock(&g_line[psp_emul_stripe(ea)]);
}

void psp_emul_line_unlock(uint64_t ea) { pthread_mutex_unlock(&g_line[psp_emul_stripe(ea)]); }

uint64_t psp_emul_version(uint64_t ea) { return g_ver[psp_emul_stripe(ea)]; }

void psp_emul_version_bump(uint64_t ea) { ++g_ver[psp_emul_stripe(ea)]; }
