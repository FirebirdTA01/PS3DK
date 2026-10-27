/* PSGL teardown must wait for queued GPU work with a fresh completion token.
 *
 * Each case queues a GPU wait on a test label that a PPU thread releases
 * later, followed by a back-end write of a "done" label.  Work queued before
 * the teardown call has completed only once "done" holds its value.
 *
 * - psglFreeCommandBuffer returns only after the queued work completed, and
 *   the buffer is no longer GPU-mapped afterwards.
 * - If the GPU does not complete within the teardown bound, the call reports
 *   GL_INVALID_OPERATION and keeps the buffer mapped and allocated; freeing it
 *   again after the GPU drained succeeds.
 * - psglExit waits for queued work even when the application already used
 *   reference value 1 itself (the value a stale-equality Finish would reuse).
 *   Built with TEARDOWN_EXIT_TIMEOUT, psglExit instead hits its bound, stops,
 *   and a later psglExit completes the teardown.
 */
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <cell/gcm.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/timer.h>

SYS_PROCESS_PARAM(1001, 0x100000);

/* Internal counter of teardown waits that hit their bound (not public API).
   Weak, so a library without it links and the checks fail at run time. */
extern unsigned psgl_teardown_wait_timeouts __attribute__((weak));

#define GATE_LABEL 201
#define DONE_LABEL 202
#define CB_BYTES (1024u * 1024u)
#define BOUND_MS 2000u

static unsigned checks, failures;
static volatile uint32_t *gate, *done;

static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_TEARDOWN_FAIL %s\n", name); }
}

static unsigned timeouts(void)
{
    return &psgl_teardown_wait_timeouts != 0 ? psgl_teardown_wait_timeouts : 0u;
}

static uint64_t now_ms(void)
{
    return (uint64_t)psglGetSystemTime() / 1000u;
}

static void releaser(uint64_t delay_us)
{
    sys_timer_usleep((uint32_t)delay_us);
    __sync_synchronize();
    *gate = 1u;
    __sync_synchronize();
    sys_ppu_thread_exit(0);
}

static int start_release(sys_ppu_thread_t *thread, uint64_t delay_us)
{
    return sys_ppu_thread_create(thread, releaser, delay_us, 1000, 0x4000,
                                 SYS_PPU_THREAD_CREATE_JOINABLE, "gate") == 0;
}

static void join_release(sys_ppu_thread_t thread)
{
    uint64_t status = 0;
    sys_ppu_thread_join(thread, &status);
}

/* GPU: wait for gate == 1, then write done = value. */
static void queue_gated(uint32_t value)
{
    *gate = 0u;
    *done = 0u;
    __sync_synchronize();
    cellGcmSetWaitLabel(gCellGcmCurrentContext, GATE_LABEL, 1u);
    cellGcmSetWriteBackEndLabel(gCellGcmCurrentContext, DONE_LABEL, value);
    cellGcmFlush(gCellGcmCurrentContext);
}

static int mapped(void *address)
{
    uint32_t offset = 0;
    return cellGcmAddressToOffset(address, &offset) == 0;
}

static void *recorded_buffer(void)
{
    GLuint offset = 0;
    void *buffer = psglAllocateCommandBuffer(CB_BYTES, &offset);
    if (!buffer) return NULL;
    psglBeginCommandRecord(buffer, CB_BYTES);
    psglEndCommandRecord(PSGL_ADD_RETURN);
    return buffer;
}

static void free_waits_and_unmaps(void)
{
    sys_ppu_thread_t thread;
    void *buffer = recorded_buffer();
    check(buffer != NULL, "allocate command buffer");
    if (!buffer) return;
    psglCallCommandBuffer(buffer);
    queue_gated(11u);
    unsigned before = timeouts();
    if (!start_release(&thread, 300000u)) { check(0, "start releaser"); return; }
    uint64_t t0 = now_ms();
    psglFreeCommandBuffer(buffer);
    uint64_t elapsed = now_ms() - t0;
    uint32_t seen = *done;
    GLenum error = glGetError();
    int still_mapped = mapped(buffer);
    join_release(thread);
    glFinish();
    printf("PSGL_TEARDOWN_FREE elapsed_ms=%llu done=%u error=0x%x mapped=%d timeouts=%u\n",
           (unsigned long long)elapsed, (unsigned)seen, (unsigned)error, still_mapped,
           timeouts() - before);
    check(seen == 11u, "psglFreeCommandBuffer waits for queued GPU work");
    check(error == GL_NO_ERROR, "psglFreeCommandBuffer reports no error when the GPU drains");
    check(!still_mapped, "psglFreeCommandBuffer unmaps the buffer");
    check(&psgl_teardown_wait_timeouts != 0 && timeouts() == before,
          "no teardown timeout when the GPU drains in time");
}

static void free_timeout_keeps_buffer(void)
{
    sys_ppu_thread_t thread;
    void *buffer = recorded_buffer();
    check(buffer != NULL, "allocate second command buffer");
    if (!buffer) return;
    psglCallCommandBuffer(buffer);
    queue_gated(12u);
    unsigned before = timeouts();
    if (!start_release(&thread, (BOUND_MS + 600u) * 1000u)) { check(0, "start releaser"); return; }
    uint64_t t0 = now_ms();
    psglFreeCommandBuffer(buffer);
    uint64_t elapsed = now_ms() - t0;
    GLenum error = glGetError();
    int still_mapped = mapped(buffer);
    uint32_t first_word = ((volatile uint32_t *)buffer)[0];
    printf("PSGL_TEARDOWN_FREE_TIMEOUT elapsed_ms=%llu error=0x%x mapped=%d word0=0x%x timeouts=%u\n",
           (unsigned long long)elapsed, (unsigned)error, still_mapped, (unsigned)first_word,
           timeouts() - before);
    check(elapsed + 100u >= BOUND_MS, "psglFreeCommandBuffer waits up to its bound");
    check(error == GL_INVALID_OPERATION, "a timed-out free reports GL_INVALID_OPERATION");
    check(still_mapped, "a timed-out free keeps the buffer mapped");
    check(first_word == 0u, "a timed-out free keeps the buffer allocated");
    check(&psgl_teardown_wait_timeouts != 0 && timeouts() == before + 1u,
          "a timed-out free counts one teardown timeout");
    join_release(thread);
    glFinish();
    check(*done == 12u, "gated work completes after release");
    /* Free again only if the first call kept the buffer; freeing a buffer a
       library already released would be a double free. */
    int kept = error == GL_INVALID_OPERATION && still_mapped;
    if (kept) psglFreeCommandBuffer(buffer);
    check(kept && glGetError() == GL_NO_ERROR, "freeing again after the GPU drained succeeds");
    check(kept && !mapped(buffer), "freeing again unmaps the buffer");
}

int main(void)
{
    PSGLinitOptions options = {0};
    psglInit(&options);
    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglCreateContext();
    if (!device || !context) { puts("PSGL_TEARDOWN_FAIL setup"); return 1; }
    psglMakeCurrent(context, device);
    gate = (volatile uint32_t *)cellGcmGetLabelAddress(GATE_LABEL);
    done = (volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL);

    /* A frame so the device has a submitted flip at teardown. */
    glClearColor(0.0f, 0.2f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    psglSwap();
    glFinish();

    free_waits_and_unmaps();
    free_timeout_keeps_buffer();

    /* The application's own Finish leaves reference value 1 behind. */
    cellGcmFinish(gCellGcmCurrentContext, 1u);
    sys_ppu_thread_t thread;
    unsigned before = timeouts();
#ifdef TEARDOWN_EXIT_TIMEOUT
    queue_gated(14u);
    if (!start_release(&thread, (BOUND_MS + 600u) * 1000u)) { puts("PSGL_TEARDOWN_FAIL releaser"); return 1; }
    uint64_t t0 = now_ms();
    psglExit();
    uint64_t elapsed = now_ms() - t0;
    unsigned counted = timeouts() - before;
    uint32_t seen = *done;
    join_release(thread);
    uint64_t t1 = now_ms();
    psglExit();
    uint64_t elapsed2 = now_ms() - t1;
    printf("PSGL_TEARDOWN_EXIT_TIMEOUT elapsed_ms=%llu done=%u timeouts=%u second_ms=%llu done_after=%u\n",
           (unsigned long long)elapsed, (unsigned)seen, counted,
           (unsigned long long)elapsed2, (unsigned)*done);
    check(elapsed + 100u >= BOUND_MS, "psglExit waits up to its bound");
    check(seen != 14u, "the gated work was still pending at the bound");
    check(&psgl_teardown_wait_timeouts != 0 && counted >= 1u, "psglExit counts the teardown timeout");
    check(*done == 14u, "a second psglExit completes after the GPU drains");
#else
    queue_gated(13u);
    if (!start_release(&thread, 300000u)) { puts("PSGL_TEARDOWN_FAIL releaser"); return 1; }
    uint64_t t0 = now_ms();
    psglExit();
    uint64_t elapsed = now_ms() - t0;
    uint32_t seen = *done;
    join_release(thread);
    printf("PSGL_TEARDOWN_EXIT elapsed_ms=%llu done=%u timeouts=%u\n",
           (unsigned long long)elapsed, (unsigned)seen, timeouts() - before);
    check(seen == 13u, "psglExit waits for queued work despite a stale reference value");
    check(&psgl_teardown_wait_timeouts != 0 && timeouts() == before,
          "psglExit does not time out when the GPU drains in time");
#endif
    printf("PSGL_TEARDOWN_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
