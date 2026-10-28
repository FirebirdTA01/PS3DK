/* Three independent cellAudio ports; libmixer is not used (it is a placeholder). */
#include "display.h"
#include "../../../gcm/common/gcm_host_buffer.h"
#include <cell/audio.h>
#include <sys/ppu_thread.h>
#include <sys/event_queue.h>
#include <math.h>
#include <string.h>

SYS_PROCESS_PARAM(1001, 0x100000);
enum { STREAMS=3, PHASE_COUNT=4, PHASE_BLOCKS=375, RATE=48000, SAMPLES=256 };
static const float frequencies[STREAMS]={261.6256f,329.6276f,391.9954f};
static const float gains[PHASE_COUNT][STREAMS]={
    {0.80f,0.15f,0.05f}, {0.05f,0.80f,0.15f},
    {0.15f,0.05f,0.80f}, {0.45f,0.45f,0.45f}
};
static const float pan[STREAMS][2]={{1.0f,0.35f},{0.70f,0.70f},{0.35f,1.0f}};
typedef struct Stream {
    uint32_t port;
    CellAudioPortConfig config;
    float oscillator;
    unsigned submitted;
    int opened,started;
} Stream;
static Stream streams[STREAMS];
static float pcm[SAMPLES*2] __attribute__((aligned(16)));
static sys_event_queue_t queue;
static sys_ipc_key_t queue_key;
static atomic_uint audio_stop,audio_bad,worker_done,phase_id,submitted_blocks;
static unsigned completed_phases;

static int checked(int rc,const char *what)
{
    if (!rc) return 1;
    printf("AUDIO_MIX_ERROR operation=%s rc=%08x\n",what,(unsigned)rc);
    atomic_store_explicit(&audio_bad,1,memory_order_release);
    return 0;
}
#define CALL(x) checked((x),#x)

static int stopping(void)
{
    return atomic_load_explicit(&audio_stop,memory_order_acquire) ||
           atomic_load_explicit(&quit_requested,memory_order_acquire);
}

static int make_block(unsigned n)
{
    Stream *s=&streams[n];
    const float step=frequencies[n]/RATE;
    for (unsigned i=0;i<SAMPLES;++i) {
        float value=0.10f*sinf(s->oscillator*6.283185307179586f);
        if (!isfinite(value) || fabsf(value)>0.100001f) {
            puts("AUDIO_MIX_ERROR PCM outside finite amplitude bound");
            atomic_store(&audio_bad,1); return 0;
        }
        pcm[2*i]=value*pan[n][0]; pcm[2*i+1]=value*pan[n][1];
        s->oscillator+=step;
        if (s->oscillator>=1.0f) s->oscillator-=1.0f;
    }
    return 1;
}

static int tick(void)
{
    sys_event_t event;
    /* A timeout is a failed heartbeat, never permission to write another block. */
    return CALL(sysEventQueueReceive(queue,&event,250000));
}

static void feed_audio(uint64_t argument)
{
    (void)argument;
    uint64_t prior[STREAMS]={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    for (unsigned phase=0;phase<PHASE_COUNT && !stopping();++phase) {
        unsigned progress[STREAMS]={0},duplicates[STREAMS]={0};
        for (unsigned n=0;n<STREAMS;++n)
            if (!CALL(cellAudioSetPortLevel(streams[n].port,gains[phase][n]))) goto finish;
        atomic_store_explicit(&phase_id,phase,memory_order_release);
        printf("AUDIO_MIX_BEGIN phase=%u gains=%.2f,%.2f,%.2f blocks=%u\n",
               phase,gains[phase][0],gains[phase][1],gains[phase][2],PHASE_BLOCKS);
        uint64_t start=now_us();
        unsigned b;
        for (b=0;b<PHASE_BLOCKS && !stopping();++b) {
            if (!tick()) goto finish;
            if (stopping()) break;
            for (unsigned n=0;n<STREAMS;++n) {
                Stream *s=&streams[n];
                volatile uint64_t *read_index=(volatile uint64_t *)(uintptr_t)s->config.readIndexAddr;
                uint64_t index=*read_index;
                if (index>=s->config.nBlock) {
                    puts("AUDIO_MIX_ERROR invalid port read index");
                    atomic_store(&audio_bad,1); goto finish;
                }
                if (index==prior[n]) ++duplicates[n]; else ++progress[n];
                prior[n]=index;
                if (!make_block(n) || !CALL(cellAudioAdd2chData(s->port,pcm,SAMPLES,1.0f))) goto finish;
                ++s->submitted;
            }
            atomic_store_explicit(&submitted_blocks,phase*PHASE_BLOCKS+b+1,memory_order_release);
        }
        if (stopping()) break;
        if (b!=PHASE_BLOCKS || !progress[0] || !progress[1] || !progress[2]) {
            puts("AUDIO_MIX_ERROR phase made no port progress");
            atomic_store(&audio_bad,1); goto finish;
        }
        printf("AUDIO_MIX_PHASE phase=%u blocks=%u progress=%u,%u,%u duplicate_indices=%u,%u,%u elapsed_us=%" PRIu64 "\n",
               phase,b,progress[0],progress[1],progress[2],duplicates[0],duplicates[1],duplicates[2],now_us()-start);
        ++completed_phases;
    }
    /* Ramp down each port using its volume control; queued samples may remain. */
    for (unsigned n=0;n<STREAMS;++n)
        if (!CALL(cellAudioSetPortLevel(streams[n].port,0.0f))) goto finish;
    memset(pcm,0,sizeof(pcm));
    for (unsigned b=0;b<10 && !stopping();++b) {
        if (!tick()) goto finish;
        for (unsigned n=0;n<STREAMS;++n)
            if (!CALL(cellAudioAdd2chData(streams[n].port,pcm,SAMPLES,1.0f))) goto finish;
    }
finish:
    atomic_store_explicit(&worker_done,1,memory_order_release);
    sys_ppu_thread_exit(atomic_load(&audio_bad)?1:0);
}

int main(void)
{
    int audio_initialized=0,queue_created=0,queue_registered=0,sysutil_registered=0,thread_created=0;
    sys_ppu_thread_t thread=0;
    /* LV2 pages, not the sbrk heap; kept for the program's life once GCM
     * maps it (see gcm_host_buffer.h). */
    sys_addr_t host=0;
    if (gcm_host_buffer_alloc(HOST_BYTES,&host)!=0 || cellGcmInit(FIFO_BYTES,HOST_BYTES,(void *)(uintptr_t)host) || !display_init()) {
        puts("AUDIO_MIX_FAIL display initialization"); return 1;
    }
    CellGcmContextData *ctx=CELL_GCM_CURRENT;
    cellGcmSetFlipHandler(on_flip);
    cellGcmSetFlipMode(GCM_FLIP_VSYNC);
    if (!CALL(cellSysutilRegisterCallback(0,on_sysutil,NULL))) goto cleanup;
    sysutil_registered=1;
    if (!CALL(cellAudioInit())) goto cleanup;
    audio_initialized=1;
    for (unsigned n=0;n<STREAMS;++n) {
        Stream *s=&streams[n];
        CellAudioPortParam param={.nChannel=2,.nBlock=8,
            .attr=CELL_AUDIO_PORTATTR_BGM|CELL_AUDIO_PORTATTR_INITLEVEL,.level=0.0f};
        if (!CALL(cellAudioPortOpen(&param,&s->port))) goto cleanup;
        s->opened=1;
        if (!CALL(cellAudioGetPortConfig(s->port,&s->config))) goto cleanup;
        if (s->config.nChannel!=2 || s->config.nBlock!=8 || !s->config.portAddr ||
            !s->config.readIndexAddr || s->config.portSize!=8*2*SAMPLES*sizeof(float)) {
            puts("AUDIO_MIX_ERROR unexpected port layout"); atomic_store(&audio_bad,1); goto cleanup;
        }
        for (unsigned j=0;j<n;++j) if (streams[j].port==s->port) {
            puts("AUDIO_MIX_ERROR duplicate port"); atomic_store(&audio_bad,1); goto cleanup;
        }
        memset((void *)(uintptr_t)s->config.portAddr,0,s->config.portSize);
        printf("AUDIO_MIX_PORT stream=%u port=%u channels=2 blocks=8 tone_hz=%.4f\n",n,s->port,frequencies[n]);
    }
    if (!CALL(cellAudioCreateNotifyEventQueue(&queue,&queue_key))) goto cleanup;
    queue_created=1;
    if (!CALL(cellAudioSetNotifyEventQueue(queue_key))) goto cleanup;
    queue_registered=1;
    if (!CALL(sysEventQueueDrain(queue))) goto cleanup;
    for (unsigned n=0;n<STREAMS;++n) {
        if (!CALL(cellAudioPortStart(streams[n].port))) goto cleanup;
        streams[n].started=1;
    }
    if (!CALL(sys_ppu_thread_create(&thread,feed_audio,0,1000,0x10000,SYS_PPU_THREAD_CREATE_JOINABLE,"audio mix feed"))) goto cleanup;
    thread_created=1;
    uint64_t start=now_us();
    while (!atomic_load_explicit(&worker_done,memory_order_acquire)) {
        if (!callbacks() || atomic_load(&quit_requested)) { atomic_store(&audio_stop,1); break; }
        unsigned phase=atomic_load_explicit(&phase_id,memory_order_acquire);
        render(ctx,phase,gains[phase],atomic_load_explicit(&submitted_blocks,memory_order_acquire));
        if (!present(ctx) || now_us()-start>30000000) {
            failed=1; atomic_store(&audio_stop,1); break;
        }
    }
cleanup:
    if (thread_created) {
        atomic_store_explicit(&audio_stop,1,memory_order_release);
        uint64_t start=now_us();
        while (!atomic_load_explicit(&worker_done,memory_order_acquire) && now_us()-start<2000000) usleep(1000);
        if (!atomic_load(&worker_done)) {
            /* Do not tear down ports that a live worker might still access. */
            puts("AUDIO_MIX_FAIL worker stop timeout; process teardown required"); return 1;
        }
        uint64_t status=UINT64_MAX;
        if (!CALL(sys_ppu_thread_join(thread,&status)) || status!=0) atomic_store(&audio_bad,1);
    }
    for (unsigned n=0;n<STREAMS;++n)
        if (streams[n].started) CALL(cellAudioPortStop(streams[n].port));
    if (queue_registered) CALL(cellAudioRemoveNotifyEventQueue(queue_key));
    if (queue_created) CALL(sysEventQueueDestroy(queue,0));
    for (unsigned n=0;n<STREAMS;++n)
        if (streams[n].opened) CALL(cellAudioPortClose(streams[n].port));
    if (audio_initialized) CALL(cellAudioQuit());
    cellGcmSetFlipHandler(NULL);
    if (sysutil_registered) CALL(cellSysutilUnregisterCallback(0));
    if (failed || atomic_load(&audio_bad)) { puts("AUDIO_MIX_FAIL lifecycle or rendering"); return 1; }
    if (atomic_load(&quit_requested)) { printf("AUDIO_MIX_QUIT phases=%u\n",completed_phases); return 0; }
    if (completed_phases!=4 || streams[0].submitted!=1500 || streams[1].submitted!=1500 || streams[2].submitted!=1500) {
        puts("AUDIO_MIX_FAIL submission accounting"); return 1;
    }
    puts("AUDIO_MIX_OK phases=4 streams=3 blocks_per_stream=1500 (submission/lifecycle; audible output checked separately)");
    return 0;
}
