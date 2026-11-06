/* Automatic lifecycle, linked only into libps3prof, never the manual archive.
 * Compile this object and all runtime dependencies without compiler hooks.
 * Contract: docs/design/ps3dk-prof-auto-lifecycle.md. */
#include "../core/prof_internal.h"

static uint32_t active;

/* The executable forces this symbol with -u so archive extraction retains the
 * init/fini arrays even when no application code calls a profiler function. */
void __ps3dk_prof_startup_anchor(void) {}

/* Diagnostic output, like the capture sink, uses the PAL's unbuffered file
 * writes. In the LV2 PAL these reach librt write -> filesystem syscalls; no
 * stdio object or its teardown order is involved. Failure cannot certify a
 * capture: its absent trailer/refusal remains the reader's authority. */
static void diagnostic(const char *text, unsigned long size)
{
    int err = 0;
    while (size) {
        long n = pal_write(2, text, size, &err);
        if (n <= 0 || (unsigned long)n > size)
            break;
        text += n;
        size -= (unsigned long)n;
    }
}

__attribute__((constructor(100))) static void automatic_start(void)
{
    ps3dk_prof_config cfg;
    /* Runs after libc's runtime ladder (heap83, syscalls84, metaLock85,
     * finalization86) and libstdc++90/98, before ordinary user constructors101.
     * Libc locks are live. THREAD mode starts the drainer before user ctors
     * and main; it touches only the PAL and lock-safe libc/runtime paths.
     * Keep the priority-101 allocation/capture row passing on both PPU ABIs. */
    ps3dk_prof_config_defaults(&cfg);
    cfg.mode = PS3DK_PROF_MODE_THREAD;
    cfg.auto_register = 1;
    cfg.flags = "automatic-lifecycle";
    if (ps3dk_prof_init(&cfg) == PS3DK_PROF_OK) {
        ST_REL(&active, 1u);
    } else {
        static const char msg[] = "ps3prof: automatic initialization failed; no complete capture\n";
        diagnostic(msg, sizeof msg - 1);
    }
}

/* Private librt notification, after all user TLS/key cleanup. No key destructor
 * is installed here: it could retire before another user's destructor.
 * thread_end is inert for an unregistered/already-retired producer and never
 * allocates or registers a new producer. Raw/abandoned exits never call here. */
void __ps3dk_prof_thread_exit(void)
{
    if (LD_ACQ(&active))
        ps3dk_prof_thread_end();
}

__attribute__((destructor(100))) static void automatic_finish(void)
{
    if (LD_ACQ(&active)) {
        /* shutdown acknowledges the main producer after user destructors and
         * drains the real frontier. It preserves open spans and missing worker
         * acknowledgements; a join never invents a retirement notification. */
        int rc = ps3dk_prof_shutdown();
        ST_REL(&active, 0u);
        if (rc != PS3DK_PROF_OK) {
            static const char msg[] = "ps3prof: automatic finalization failed; inspect capture validity\n";
            diagnostic(msg, sizeof msg - 1);
        }
    }
}
