#include <ps3dk/prof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static long put(void *ctx,const void *p,unsigned long n) {(void)ctx;(void)p; return (long)n;}
static int end(void *ctx) {(void)ctx;return 0;}
int main(int argc,char **argv) {
    if(argc!=5) return 3;
    if(ps3dk_prof_set_capture_retention(1025)!=PS3DK_PROF_EINVAL) return 4;
    if(ps3dk_prof_set_capture_retention((uint32_t)strtoul(argv[3],0,10))) return 5;
    ps3dk_prof_config c; ps3dk_prof_config_defaults(&c);
    c.app=argv[2]; c.max_threads=2; c.max_descriptors=8; c.ring_records=512;
    if(!strcmp(argv[4],"thread")) c.mode=PS3DK_PROF_MODE_THREAD;
    if(!strcmp(argv[4],"callback")) {
        ps3dk_prof_sink_callbacks cb={put,NULL,end,NULL};
        if(ps3dk_prof_set_sink_callbacks(&cb)) return 6;
        return ps3dk_prof_init(&c)==PS3DK_PROF_ENOTSUP ? 0 : 7;
    }
    if(ps3dk_prof_set_sink_path(argv[1]) || ps3dk_prof_init(&c)) return 8;
    if(ps3dk_prof_set_capture_retention(0)!=PS3DK_PROF_EALREADY) return 9;
    if(ps3dk_prof_thread_begin("owner") || PS3DK_PROF_CAPTURE_WHOLE_RUN()) return 10;
    ps3dk_prof_zone z=PS3DK_PROF_ZONE_BEGIN("kept");
    PS3DK_PROF_ZONE_END(z); ps3dk_prof_frame();
    if(!strcmp(argv[4],"exit")) {ps3dk_prof_drain(); _exit(0);}
    ps3dk_prof_thread_end();
    int rc=ps3dk_prof_shutdown();
    printf("shutdown=%d\n",rc);
    return !strcmp(argv[4],"lock") ? (rc==PS3DK_PROF_ERETENTION ? 0 : 11) : (rc ? 12 : 0);
}
