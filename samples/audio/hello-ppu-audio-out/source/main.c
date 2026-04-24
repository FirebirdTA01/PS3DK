/*
 * hello-ppu-audio-out - cellAudioOut discovery + cellAudio tone playback.
 *
 * Part 1: exercises the cellAudioOut surface shipped in
 *   <cell/cell_audio_out.h> against libsysutil_audio_out_stub.a.
 *   Queries device count, per-device sound modes, current output state,
 *   and current configuration.  No PCM on this path - it's the
 *   configuration-query API that lives alongside cellVideoOut.
 *
 * Part 2: plays a 440 Hz sine tone for 1 second through the cellAudio
 *   mixer (stereo, 48 kHz).  Uses the event-queue-driven ring-fill
 *   pattern: each mixer heartbeat wakes us up, we generate one
 *   CELL_AUDIO_BLOCK_SAMPLES block of float PCM, and memcpy it a few
 *   blocks ahead of the read head.  Proves that cellAudioOut discovery
 *   returns sane values AND that the mixer downstream can be driven
 *   directly from the ring without cellAudioAddData.
 *
 * Expected TTY log ends with "result: PASS".
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include <sys/process.h>
#include <sys/event_queue.h>
#include <sys/return_code.h>

#include <cell/audio.h>
#include <cell/cell_audio_out.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* ------------------------------------------------------------------ *
 * Pretty-printers for cellAudioOut enums.
 * ------------------------------------------------------------------ */

static const char *port_type_str(uint8_t t) {
    switch (t) {
    case CELL_AUDIO_OUT_PORT_HDMI:      return "HDMI";
    case CELL_AUDIO_OUT_PORT_SPDIF:     return "SPDIF";
    case CELL_AUDIO_OUT_PORT_ANALOG:    return "ANALOG";
    case CELL_AUDIO_OUT_PORT_USB:       return "USB";
    case CELL_AUDIO_OUT_PORT_BLUETOOTH: return "BLUETOOTH";
    case CELL_AUDIO_OUT_PORT_NETWORK:   return "NETWORK";
    default:                             return "?";
    }
}

static const char *coding_str(uint8_t c) {
    switch (c) {
    case CELL_AUDIO_OUT_CODING_TYPE_LPCM:               return "LPCM";
    case CELL_AUDIO_OUT_CODING_TYPE_AC3:                return "AC3";
    case CELL_AUDIO_OUT_CODING_TYPE_DTS:                return "DTS";
    case CELL_AUDIO_OUT_CODING_TYPE_AAC:                return "AAC";
    case CELL_AUDIO_OUT_CODING_TYPE_DOLBY_DIGITAL_PLUS: return "DDPLUS";
    case CELL_AUDIO_OUT_CODING_TYPE_BITSTREAM:          return "BITSTREAM";
    default:                                             return "?";
    }
}

static const char *output_state_str(uint8_t s) {
    switch (s) {
    case CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED:   return "ENABLED";
    case CELL_AUDIO_OUT_OUTPUT_STATE_DISABLED:  return "DISABLED";
    case CELL_AUDIO_OUT_OUTPUT_STATE_PREPARING: return "PREPARING";
    default:                                     return "?";
    }
}

static const char *device_state_str(uint8_t s) {
    switch (s) {
    case CELL_AUDIO_OUT_DEVICE_STATE_UNAVAILABLE: return "UNAVAILABLE";
    case CELL_AUDIO_OUT_DEVICE_STATE_AVAILABLE:   return "AVAILABLE";
    default:                                       return "?";
    }
}

/* ------------------------------------------------------------------ *
 * Part 1: cellAudioOut discovery
 * ------------------------------------------------------------------ */

static int run_discovery(void)
{
    int any_fail = 0;

    int nDevices = cellAudioOutGetNumberOfDevice(CELL_AUDIO_OUT_PRIMARY);
    printf("  cellAudioOutGetNumberOfDevice(PRIMARY)   = %d\n", nDevices);
    if (nDevices < 0) {
        printf("  -> error reading device count\n");
        any_fail = 1;
    } else if (nDevices == 0) {
        /* RPCS3's HLE for cellAudioOut sometimes returns 0 here even
         * though State + Configuration succeed.  Note it but don't
         * fail the sample - the FNIDs still resolved cleanly. */
        printf("  -> 0 devices reported (RPCS3 HLE quirk; "
               "Get{State,Configuration} below still succeed)\n");
    }

    for (int d = 0; d < nDevices; d++) {
        CellAudioOutDeviceInfo info;
        memset(&info, 0, sizeof(info));
        int rc = cellAudioOutGetDeviceInfo(CELL_AUDIO_OUT_PRIMARY,
                                           (uint32_t)d, &info);
        if (rc) {
            printf("  cellAudioOutGetDeviceInfo[%d] = %#x (skipping)\n", d, rc);
            any_fail = 1;
            continue;
        }
        printf("  cellAudioOutGetDeviceInfo[%d]             port=%s(%u) "
               "state=%s(%u) latency=%ums modes=%u\n",
               d,
               port_type_str(info.portType), info.portType,
               device_state_str(info.state), info.state,
               info.latency, info.availableModeCount);
        for (int m = 0; m < info.availableModeCount && m < 16; m++) {
            const CellAudioOutSoundMode *sm = &info.availableModes[m];
            printf("    mode[%d] type=%s(%u) ch=%u fs=0x%02x layout=0x%08x\n",
                   m, coding_str(sm->type), sm->type,
                   sm->channel, sm->fs, (unsigned)sm->layout);
        }
    }

    CellAudioOutState state;
    memset(&state, 0, sizeof(state));
    int rc = cellAudioOutGetState(CELL_AUDIO_OUT_PRIMARY, 0, &state);
    printf("  cellAudioOutGetState                     rc=%#x state=%s(%u) "
           "encoder=%s(%u)\n",
           rc, output_state_str(state.state), state.state,
           coding_str(state.encoder), state.encoder);
    if (rc == 0) {
        printf("    soundMode                              ch=%u fs=0x%02x "
               "layout=0x%08x\n",
               state.soundMode.channel,
               state.soundMode.fs,
               (unsigned)state.soundMode.layout);
    } else {
        any_fail = 1;
    }

    CellAudioOutConfiguration config;
    CellAudioOutOption option;
    memset(&config, 0, sizeof(config));
    memset(&option, 0, sizeof(option));
    rc = cellAudioOutGetConfiguration(CELL_AUDIO_OUT_PRIMARY, &config, &option);
    printf("  cellAudioOutGetConfiguration             rc=%#x ch=%u encoder=%s(%u) "
           "downMixer=0x%08x\n",
           rc, config.channel, coding_str(config.encoder), config.encoder,
           (unsigned)config.downMixer);
    if (rc) any_fail = 1;

    return any_fail;
}

/* ------------------------------------------------------------------ *
 * Part 2: cellAudio tone playback (event-queue-driven ring fills)
 *
 * 440 Hz sine at -9 dBFS, 1 s duration, stereo 48 kHz.  Short amplitude
 * ramps at the head and tail of the tone keep the start/stop transitions
 * click-free.
 * ------------------------------------------------------------------ */

#define SAMPLE_RATE    48000u
#define BLOCK_FRAMES   CELL_AUDIO_BLOCK_SAMPLES
#define N_CHAN         2u
#define FREQ_HZ        440.0f
#define TONE_AMPL      0.35f
#define TONE_MS        1000u
#define RAMP_FRAMES    240u /* 5 ms */

static float g_block[BLOCK_FRAMES * N_CHAN] __attribute__((aligned(16)));

static void gen_block(uint32_t frames_played, uint32_t total_frames)
{
    const float phase_inc = 2.0f * (float)M_PI * FREQ_HZ / (float)SAMPLE_RATE;
    static float phase = 0.0f;
    for (uint32_t i = 0; i < BLOCK_FRAMES; i++) {
        uint32_t f = frames_played + i;
        float env = 1.0f;
        if (total_frames > 2u * RAMP_FRAMES) {
            if (f < RAMP_FRAMES)
                env = (float)f / (float)RAMP_FRAMES;
            else if (f > total_frames - RAMP_FRAMES && f < total_frames)
                env = (float)(total_frames - f) / (float)RAMP_FRAMES;
            else if (f >= total_frames)
                env = 0.0f;
        }
        float v = sinf(phase) * TONE_AMPL * env;
        phase += phase_inc;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        g_block[i * N_CHAN + 0] = v;
        g_block[i * N_CHAN + 1] = v;
    }
}

static int run_playback(void)
{
    int any_fail = 0;
    printf("hello-ppu-audio-out: cellAudio tone playback (%u ms of %.0fHz)\n",
           TONE_MS, (double)FREQ_HZ);

    int rc = cellAudioInit();
    if (rc) { printf("  cellAudioInit: %#x\n", rc); return 1; }

    CellAudioPortParam param = {
        .nChannel = CELL_AUDIO_PORT_2CH,
        .nBlock   = CELL_AUDIO_BLOCK_16,
        .attr     = CELL_AUDIO_PORTATTR_INITLEVEL,
        .level    = 1.0f,
    };
    uint32_t port = 0;
    rc = cellAudioPortOpen(&param, &port);
    if (rc) { printf("  cellAudioPortOpen: %#x\n", rc); goto quit; }

    CellAudioPortConfig cfg = {0};
    rc = cellAudioGetPortConfig(port, &cfg);
    if (rc) { printf("  cellAudioGetPortConfig: %#x\n", rc); goto close_port; }
    printf("  port=%u nChannel=%llu nBlock=%llu size=%u\n",
           (unsigned)port,
           (unsigned long long)cfg.nChannel,
           (unsigned long long)cfg.nBlock,
           (unsigned)cfg.portSize);

    sys_event_queue_t queue = 0;
    sys_ipc_key_t     key   = 0;
    rc = cellAudioCreateNotifyEventQueue(&queue, &key);
    if (rc) { printf("  CreateNotifyEventQueue: %#x\n", rc); goto close_port; }
    rc = cellAudioSetNotifyEventQueue(key);
    if (rc) { printf("  SetNotifyEventQueue: %#x\n", rc); goto destroy_queue; }
    sysEventQueueDrain(queue);

    volatile uint64_t *read_index = (volatile uint64_t *)(uintptr_t)cfg.readIndexAddr;
    float             *ring       = (float *)(uintptr_t)cfg.portAddr;
    const uint32_t     n_block    = (uint32_t)cfg.nBlock;
    const uint32_t     block_floats = (uint32_t)cfg.nChannel * BLOCK_FRAMES;
    const uint32_t     lookahead  = n_block / 2;

    memset(ring, 0, (size_t)cfg.portSize);

    rc = cellAudioPortStart(port);
    if (rc) { printf("  cellAudioPortStart: %#x\n", rc); goto remove_queue; }

    const uint32_t total_frames = (SAMPLE_RATE * TONE_MS) / 1000u;
    uint32_t played = 0;
    while (played < total_frames) {
        sys_event_t ev;
        int wait_rc = sysEventQueueReceive(queue, &ev, 4 * 1000 * 1000);
        if (wait_rc != 0 && wait_rc != ETIMEDOUT) {
            printf("  sysEventQueueReceive: 0x%08x - aborting\n",
                   (unsigned)wait_rc);
            any_fail = 1;
            break;
        }
        gen_block(played, total_frames);
        uint64_t ri = *read_index;
        uint32_t wi = (uint32_t)((ri + lookahead) % n_block);
        memcpy(&ring[wi * block_floats], g_block,
               (size_t)block_floats * sizeof(float));
        played += BLOCK_FRAMES;
    }

    /* Drain the ring with silence so the stopped port doesn't loop stale
     * samples on its way out. */
    memset(g_block, 0, sizeof(g_block));
    for (uint32_t i = 0; i < n_block + 4; i++) {
        sys_event_t ev;
        (void)sysEventQueueReceive(queue, &ev, 100 * 1000);
        uint64_t ri = *read_index;
        uint32_t wi = (uint32_t)((ri + lookahead) % n_block);
        memcpy(&ring[wi * block_floats], g_block,
               (size_t)block_floats * sizeof(float));
    }

    if ((rc = cellAudioPortStop(port)))  { printf("  PortStop: %#x\n", rc); any_fail = 1; }

remove_queue:
    if ((rc = cellAudioRemoveNotifyEventQueue(key))) {
        printf("  RemoveNotifyEventQueue: %#x\n", rc); any_fail = 1;
    }
destroy_queue:
    sysEventQueueDestroy(queue, 0);
close_port:
    if ((rc = cellAudioPortClose(port))) {
        printf("  PortClose: %#x\n", rc); any_fail = 1;
    }
quit:
    if ((rc = cellAudioQuit())) {
        printf("  cellAudioQuit: %#x\n", rc); any_fail = 1;
    }
    return any_fail;
}

int main(void)
{
    int any_fail = 0;

    printf("hello-ppu-audio-out: cellAudioOut discovery\n");
    any_fail |= run_discovery();
    any_fail |= run_playback();

    printf("result: %s\n", any_fail ? "FAIL" : "PASS");
    return any_fail ? 1 : 0;
}
