/*
 * hello-ppu-audio - libaudio stub-archive validator.
 *
 * Exercises the cellAudio SPRX surface end-to-end over our
 * nidgen-generated libaudio_stub.a:
 *
 *   cellAudioInit          -> 0x0b168f92
 *   cellAudioPortOpen      -> 0x24769d4d   (NID lookup via .rodata.sceFNID)
 *   cellAudioGetPortConfig -> 0x74a66af0
 *   cellAudioPortStart     -> 0x98437f8f
 *   cellAudioPortStop      -> 0x2fe1b98b
 *   cellAudioPortClose     -> 0x89be28f2
 *   cellAudioQuit          -> 0xfc7f5e09
 *
 * Runtime expectation under RPCS3:
 *   1. Init the audio subsystem.
 *   2. Open a 2-channel / 16-block port in BGM mode at unity level.
 *   3. Read back the port's config (mixer ring addr + size).
 *   4. Start the port, sleep briefly (mixer can run without PCM fed
 *      by the app - it just outputs silence), stop, close, quit.
 *
 * All return codes should be 0x00000000.  Any non-zero code is the
 * cellAudio-facility error space (0x80310701..0x8031070f) and gets
 * printed with the status byte preserved.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/process.h>

#include <cell/audio.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const char *rc_str(int rc) {
    switch ((unsigned)rc) {
    case 0:                                   return "OK";
    case CELL_AUDIO_ERROR_ALREADY_INIT:       return "ALREADY_INIT";
    case CELL_AUDIO_ERROR_AUDIOSYSTEM:        return "AUDIOSYSTEM";
    case CELL_AUDIO_ERROR_NOT_INIT:           return "NOT_INIT";
    case CELL_AUDIO_ERROR_PARAM:              return "PARAM";
    case CELL_AUDIO_ERROR_PORT_FULL:          return "PORT_FULL";
    case CELL_AUDIO_ERROR_PORT_ALREADY_RUN:   return "PORT_ALREADY_RUN";
    case CELL_AUDIO_ERROR_PORT_NOT_OPEN:      return "PORT_NOT_OPEN";
    case CELL_AUDIO_ERROR_PORT_NOT_RUN:       return "PORT_NOT_RUN";
    case CELL_AUDIO_ERROR_TRANS_EVENT:        return "TRANS_EVENT";
    case CELL_AUDIO_ERROR_PORT_OPEN:          return "PORT_OPEN";
    case CELL_AUDIO_ERROR_EVENT_QUEUE:        return "EVENT_QUEUE";
    default:                                  return "other";
    }
}

#define CHECK(call) do { \
    int __rc = (call); \
    printf("  %-32s = 0x%08x (%s)\n", #call, (unsigned)__rc, rc_str(__rc)); \
    if (__rc != 0) { any_fail = 1; } \
} while (0)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int any_fail = 0;

    printf("hello-ppu-audio: cellAudio init/open/start/stop/close/quit\n");

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
    printf("  -> port: readIndexAddr=0x%08x portAddr=0x%08x\n",
           (unsigned)cfg.readIndexAddr,
           (unsigned)cfg.portAddr);

    CHECK(cellAudioPortStart(port));

    /* Let the mixer run for ~250 ms; it outputs silence because no PCM
     * has been fed in.  Enough time for RPCS3's audio backend to set up
     * the output stream and confirm the port is actually running. */
    usleep(250 * 1000);

    CHECK(cellAudioPortStop(port));
    CHECK(cellAudioPortClose(port));
    CHECK(cellAudioQuit());

    printf("result: %s\n", any_fail ? "FAIL" : "PASS");
    return any_fail ? 1 : 0;
}
