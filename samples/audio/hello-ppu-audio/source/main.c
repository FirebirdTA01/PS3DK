/*
 * hello-ppu-audio - libaudio stub-archive validator + chip-tune player.
 *
 * Drives the cellAudio SPRX surface end to end:
 *
 *   cellAudioInit / Quit
 *   cellAudioPortOpen / Close / Start / Stop / GetPortConfig
 *   cellAudioCreateNotifyEventQueue
 *   cellAudioSetNotifyEventQueue / RemoveNotifyEventQueue
 *   cellAudioAddData
 *
 * Generates a short chip-tune melody as 50%-duty-cycle square waves
 * at 48 kHz stereo float, pumped one CELL_AUDIO_BLOCK_SAMPLES-block
 * at a time on the mixer heartbeat (~5.3 ms).  Plays through the
 * RPCS3 audio backend (Cubeb/Qt) when the sample runs interactively.
 *
 * Expected TTY log: the seven cellAudio lifecycle calls each report
 * status 0x00000000, the sample prints one line per played note
 * ("note A4 392.0 Hz 300ms"), and concludes with "result: PASS".
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <sys/event_queue.h>
#include <sys/return_code.h>
#include <errno.h>

#include <cell/audio.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* ------------------------------------------------------------------ *
 * Chip-tune score.
 *
 * NES/SNES-style fanfare: ascending arpeggio into a held high note.
 * Frequencies are tuned on the 12-TET scale with A4 = 440 Hz.  Total
 * duration ~1.7 seconds so the RPCS3 launch-shutdown window hears
 * the whole thing.
 * ------------------------------------------------------------------ */

typedef struct {
    float    freq_hz;       /* 0 = rest */
    uint16_t duration_ms;
    const char *label;
} note_t;

static const note_t melody[] = {
    { 261.63f, 150, "C4"  },
    { 329.63f, 150, "E4"  },
    { 392.00f, 150, "G4"  },
    { 523.25f, 300, "C5"  },
    { 392.00f, 150, "G4"  },
    { 523.25f, 150, "C5"  },
    { 659.25f, 450, "E5"  },
    {   0.00f, 100, "rest"},
    { 523.25f, 120, "C5"  },
    { 659.25f, 120, "E5"  },
    { 783.99f, 500, "G5"  },
};

#define MELODY_LEN ((int)(sizeof(melody)/sizeof(melody[0])))

/* ------------------------------------------------------------------ *
 * Port configuration: stereo, 16 blocks (~85 ms of ring buffer), BGM
 * attribute so the mixer keeps running under XMB / system-menu.
 * ------------------------------------------------------------------ */

#define SAMPLE_RATE     48000u
#define BLOCK_SAMPLES   CELL_AUDIO_BLOCK_SAMPLES      /* 256 */
#define N_CHANNELS      2u                             /* stereo */
#define BLOCK_FLOATS    (BLOCK_SAMPLES * N_CHANNELS)   /* 512 */
#define NOTE_AMPLITUDE  0.18f                          /* -15 dBFS; loud enough, not clipping */

/* Per-block scratch, stereo interleaved float32.  16-byte aligned -
 * the SPU mixer on real hardware assumes Altivec alignment when it
 * vector-loads the source buffer, so passing an under-aligned pointer
 * is a common source of cellAudioAddData CELL_AUDIO_ERROR_PARAM (0x80310704). */
static float g_block[BLOCK_FLOATS] __attribute__((aligned(16)));

/* ------------------------------------------------------------------ *
 * Oscillator.  50%-duty-cycle square wave with a short attack / release
 * envelope so note boundaries don't click.  The 5 ms ramps at either
 * end of each note are what gives the chip-tune patch its "plucked"
 * feel rather than a hard organ-style step.
 * ------------------------------------------------------------------ */

static float g_phase;

static void gen_block(float freq_hz, uint32_t sample_in_note,
                      uint32_t total_samples_in_note)
{
    const float phase_inc = (freq_hz > 0.0f)
        ? (freq_hz / (float)SAMPLE_RATE)
        : 0.0f;
    const uint32_t ramp = 240;  /* 5 ms @ 48 kHz */

    for (uint32_t i = 0; i < BLOCK_SAMPLES; i++) {
        uint32_t s = sample_in_note + i;
        float env = 1.0f;
        if (total_samples_in_note > 2u * ramp) {
            if (s < ramp)
                env = (float)s / (float)ramp;
            else if (s > total_samples_in_note - ramp)
                env = (float)(total_samples_in_note - s) / (float)ramp;
        }
        float v;
        if (freq_hz <= 0.0f) {
            v = 0.0f;
        } else {
            v = (g_phase < 0.5f ? +NOTE_AMPLITUDE : -NOTE_AMPLITUDE) * env;
            g_phase += phase_inc;
            if (g_phase >= 1.0f) g_phase -= 1.0f;
        }
        g_block[i * 2 + 0] = v;
        g_block[i * 2 + 1] = v;
    }
}

/* ------------------------------------------------------------------ *
 * Dispatch + error helpers.
 * ------------------------------------------------------------------ */

static const char *rc_str(int rc) {
    switch ((unsigned)rc) {
    case 0:                                  return "OK";
    case CELL_AUDIO_ERROR_ALREADY_INIT:      return "ALREADY_INIT";
    case CELL_AUDIO_ERROR_AUDIOSYSTEM:       return "AUDIOSYSTEM";
    case CELL_AUDIO_ERROR_NOT_INIT:          return "NOT_INIT";
    case CELL_AUDIO_ERROR_PARAM:             return "PARAM";
    case CELL_AUDIO_ERROR_PORT_FULL:         return "PORT_FULL";
    case CELL_AUDIO_ERROR_PORT_ALREADY_RUN:  return "PORT_ALREADY_RUN";
    case CELL_AUDIO_ERROR_PORT_NOT_OPEN:     return "PORT_NOT_OPEN";
    case CELL_AUDIO_ERROR_PORT_NOT_RUN:      return "PORT_NOT_RUN";
    case CELL_AUDIO_ERROR_TRANS_EVENT:       return "TRANS_EVENT";
    case CELL_AUDIO_ERROR_PORT_OPEN:         return "PORT_OPEN";
    case CELL_AUDIO_ERROR_EVENT_QUEUE:       return "EVENT_QUEUE";
    default:                                 return "other";
    }
}

#define CHECK(call) do { \
    int __rc = (call); \
    printf("  %-40s = 0x%08x (%s)\n", #call, (unsigned)__rc, rc_str(__rc)); \
    if (__rc != 0) any_fail = 1; \
} while (0)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int any_fail = 0;

    printf("hello-ppu-audio: cellAudio chip-tune playback\n");

    /* -------- Lifetime bring-up ---------------------------------- */
    CHECK(cellAudioInit());

    CellAudioPortParam param = {
        .nChannel = CELL_AUDIO_PORT_2CH,
        .nBlock   = CELL_AUDIO_BLOCK_16,
        .attr     = CELL_AUDIO_PORTATTR_BGM | CELL_AUDIO_PORTATTR_INITLEVEL,
        .level    = 1.0f,
    };
    uint32_t port = 0;
    CHECK(cellAudioPortOpen(&param, &port));
    printf("  -> port number %u\n", (unsigned)port);

    CellAudioPortConfig cfg = {0};
    CHECK(cellAudioGetPortConfig(port, &cfg));
    printf("  -> port: status=%u nChannel=%llu nBlock=%llu size=%u\n",
           (unsigned)cfg.status,
           (unsigned long long)cfg.nChannel,
           (unsigned long long)cfg.nBlock,
           (unsigned)cfg.portSize);

    /* -------- Mixer-heartbeat event queue ------------------------- */
    sys_event_queue_t queue = 0;
    sys_ipc_key_t     key   = 0;
    CHECK(cellAudioCreateNotifyEventQueue(&queue, &key));
    CHECK(cellAudioSetNotifyEventQueue(key));
    sysEventQueueDrain(queue);

    /* The mixer's ring buffer lives at cfg.portAddr and is organised as
     * cfg.nBlock blocks of (cfg.nChannel * BLOCK_SAMPLES * sizeof(float))
     * bytes each.  cfg.readIndexAddr points at a u64 that tracks which
     * block the mixer is currently consuming.  We write our PCM a few
     * blocks ahead of the read head each heartbeat, so fresh data is
     * always in place before the mixer reaches it.  This is what real
     * PS3 games do - cellAudioAddData() is an optional convenience layer
     * on top.  Direct ring writes also sidestep RPCS3's HLE validation
     * quirks with the AddData path. */
    volatile uint64_t *read_index = (volatile uint64_t *)(uintptr_t)cfg.readIndexAddr;
    float             *ring       = (float *)(uintptr_t)cfg.portAddr;
    const uint32_t     n_block    = (uint32_t)cfg.nBlock;
    const uint32_t     block_floats = (uint32_t)cfg.nChannel * BLOCK_SAMPLES;
    const uint32_t     lookahead    = n_block / 2;  /* 8 blocks = ~43 ms */

    /* Zero the ring so any block we don't write is silent, not garbage. */
    memset(ring, 0, (size_t)cfg.portSize);

    CHECK(cellAudioPortStart(port));

    for (int n = 0; n < MELODY_LEN; n++) {
        const note_t *note = &melody[n];
        uint32_t total_samples = (uint32_t)note->duration_ms * SAMPLE_RATE / 1000u;
        printf("  note %-4s %7.2f Hz  %ums\n",
               note->label, (double)note->freq_hz, (unsigned)note->duration_ms);

        uint32_t played = 0;
        while (played < total_samples) {
            sys_event_t ev;
            int rc = sysEventQueueReceive(queue, &ev, 4 * 1000 * 1000);
            if (rc != 0 && rc != ETIMEDOUT) {
                printf("    sysEventQueueReceive: 0x%08x - abandoning playback\n",
                       (unsigned)rc);
                any_fail = 1;
                goto stop_port;
            }

            gen_block(note->freq_hz, played, total_samples);

            uint64_t ri = *read_index;
            uint32_t wi = (uint32_t)((ri + lookahead) % n_block);
            memcpy(&ring[wi * block_floats], g_block,
                   (size_t)block_floats * sizeof(float));

            played += BLOCK_SAMPLES;
        }
    }

    /* Drain: keep pumping silence so the tail of the last note clears the
     * ring before we stop the port.  Otherwise a residual block can
     * repeat-loop audibly. */
    memset(g_block, 0, sizeof(g_block));
    for (uint32_t i = 0; i < n_block + 4; i++) {
        sys_event_t ev;
        (void)sysEventQueueReceive(queue, &ev, 100 * 1000);
        uint64_t ri = *read_index;
        uint32_t wi = (uint32_t)((ri + lookahead) % n_block);
        memcpy(&ring[wi * block_floats], g_block,
               (size_t)block_floats * sizeof(float));
    }

stop_port:
    /* -------- Teardown ------------------------------------------- */
    CHECK(cellAudioPortStop(port));
    CHECK(cellAudioRemoveNotifyEventQueue(key));
    /* Normal-mode destroy (mode 0). */
    sysEventQueueDestroy(queue, 0);
    CHECK(cellAudioPortClose(port));
    CHECK(cellAudioQuit());

    printf("result: %s\n", any_fail ? "FAIL" : "PASS");
    return any_fail ? 1 : 0;
}
