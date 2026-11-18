/* Automatic EXT/FIFO/AUTO lifecycle. Application TUs need no profiler calls.
 * The selector forces the anchor and supplies pinned pre-link identities. */
#include "auto_internal.h"
#include "prof_spu.h"

static uint32_t active;
void __ps3dk_prof_auto_startup_anchor(void) {}

static void diagnostic(const char *text, unsigned long size)
{
    int error=0;
    while(size) {
        long n=pal_write(2,text,size,&error);
        if(n<=0 || (unsigned long)n>size) break;
        text+=n;size-=(unsigned long)n;
    }
}

/* PS3_PROF_AUTO_RING=ON: the executable's generated constant (cmake/ps3-prof-auto.cmake, prof_auto.h). Absent,
 * nothing changes; present but refused, the automatic capture is not initialized (never a run without the ring). */
extern const ps3dk_prof_auto_ring_options ps3dk_prof_automatic_ring_options __attribute__((weak));
static int automatic_ring(void)
{
    static const char message[]="ps3prof: PS3_PROF_AUTO_RING options refused (blocks 2..3, 1..256 whole MiB, "
                                "1..65536 records); automatic capture not initialized\n";
    if(!&ps3dk_prof_automatic_ring_options ||
       ps3dk_prof__auto_build_ring(&ps3dk_prof_automatic_ring_options)==PS3DK_PROF_OK) return 1;
    diagnostic(message,sizeof message-1);
    return 0;
}

__attribute__((constructor(100))) static void automatic_start(void)
{
    ps3dk_prof_config cfg;
    ps3dk_prof_ext_config ext={0};
    ps3dk_prof_config_defaults(&cfg);
    cfg.mode=PS3DK_PROF_MODE_THREAD;
    cfg.auto_register=1;
    cfg.app=ps3dk_prof__auto_app;
    cfg.build_id=ps3dk_prof__auto_build_id;
    cfg.flags="automatic-rsx";
    ext.size=PS3DK_PROF_EXT_CONFIG_SIZE;
    ext.gpu_domains_max=1;
    ext.gpu_report_slots_max=1024;
    if(!automatic_ring()) return;
    if(ps3dk_prof_init_ex(&cfg,&ext)==PS3DK_PROF_OK) {
        ST_REL(&active,1u);
        (void)ps3dk_prof_thread_begin("automatic-init");
    } else {
        static const char message[]="ps3prof: automatic initialization failed; no complete capture\n";
        diagnostic(message,sizeof message-1);
    }
}

/* librt invokes this after user TLS/key cleanup, as for automatic zones.
 * No registration or new allocation is performed by retirement. */
void __ps3dk_prof_thread_exit(void)
{
    if(LD_ACQ(&active)) ps3dk_prof_thread_end();
}

__attribute__((destructor(100))) static void automatic_finish(void)
{
    if(LD_ACQ(&active)) {
        int rc=ps3dk_prof_shutdown();
        ST_REL(&active,0u);
        if(rc!=PS3DK_PROF_OK && rc!=PS3DK_PROF_EALREADY) {
            static const char message[]="ps3prof: automatic finalization failed; inspect capture validity\n";
            diagnostic(message,sizeof message-1);
        }
    }
}
