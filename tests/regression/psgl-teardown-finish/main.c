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
 * - If unmapping fails, the call reports GL_INVALID_OPERATION and keeps the
 *   buffer registered; freeing it again once it can be unmapped succeeds.
 * - TEARDOWN_EXIT_MODE 0: psglExit waits for queued work even when the
 *   application already used reference value 1 itself (the value a
 *   stale-equality Finish would reuse).
 * - Modes 1-3: one psglExit wait hits its bound (1: the first device wait,
 *   2: the last device wait, after the display buffers were released, 3: the
 *   shutdown wait), so psglExit stops without shutting down; a later
 *   psglExit waits on a fresh token and shuts down.
 */
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <cell/gcm.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/timer.h>

#ifndef TEARDOWN_EXIT_MODE
#define TEARDOWN_EXIT_MODE 0
#endif

SYS_PROCESS_PARAM(1001, 0x100000);

/* libPSGL internals for regression tests (not public API). Weak, so a
   library without them links and the checks fail at run time. */
extern unsigned psgl_teardown_wait_timeouts __attribute__((weak));
extern unsigned psgl_teardown_shutdowns __attribute__((weak));
extern void (*psgl_teardown_test_hook)(unsigned stage) __attribute__((weak));

#define GATE_LABEL 201
#define DONE_LABEL 202
#define PSGL_LABEL 255
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

static unsigned shutdowns(void)
{
    return &psgl_teardown_shutdowns != 0 ? psgl_teardown_shutdowns : 0u;
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

#if TEARDOWN_EXIT_MODE >= 1
/* Polls for done == value without a GL context (after psglExit). */
static int wait_done(uint32_t value)
{
    for (unsigned i = 0; i < 3000u && *done != value; i++)
        sys_timer_usleep(1000);
    return *done == value;
}
#endif

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
    /* Only a buffer the library kept may be read or freed again; a library
       that released it would make either a use after free. */
    int kept = error == GL_INVALID_OPERATION && still_mapped;
    uint32_t first_word = kept ? ((volatile uint32_t *)buffer)[0] : 0xFFFFFFFFu;
    printf("PSGL_TEARDOWN_FREE_TIMEOUT elapsed_ms=%llu error=0x%x mapped=%d kept=%d word0=0x%x timeouts=%u\n",
           (unsigned long long)elapsed, (unsigned)error, still_mapped, kept,
           (unsigned)first_word, timeouts() - before);
    check(elapsed + 100u >= BOUND_MS, "psglFreeCommandBuffer waits up to its bound");
    check(error == GL_INVALID_OPERATION, "a timed-out free reports GL_INVALID_OPERATION");
    check(still_mapped, "a timed-out free keeps the buffer mapped");
    check(kept && first_word == 0u, "a timed-out free keeps the buffer allocated");
    check(&psgl_teardown_wait_timeouts != 0 && timeouts() == before + 1u,
          "a timed-out free counts one teardown timeout");
    join_release(thread);
    glFinish();
    check(*done == 12u, "gated work completes after release");
    if (kept) psglFreeCommandBuffer(buffer);
    check(kept && glGetError() == GL_NO_ERROR, "freeing again after the GPU drained succeeds");
    check(kept && !mapped(buffer), "freeing again unmaps the buffer");
}

/* The test unmaps the buffer itself, so the library's unmap fails. */
static void free_unmap_failure_keeps_buffer(void)
{
    void *buffer = recorded_buffer();
    check(buffer != NULL, "allocate third command buffer");
    if (!buffer) return;
    unsigned before = timeouts();
    int32_t own_unmap = cellGcmUnmapEaIoAddress(buffer);
    psglFreeCommandBuffer(buffer);
    GLenum error = glGetError();
    int kept = own_unmap == 0 && error == GL_INVALID_OPERATION;
    GLenum again = GL_NO_ERROR;
    int32_t remap = -1;
    GLenum final_error = GL_INVALID_ENUM;
    int final_mapped = 1;
    if (kept) {
        psglFreeCommandBuffer(buffer);   /* still registered, still unmapped */
        again = glGetError();
        uint32_t offset = 0;
        remap = cellGcmMapMainMemory(buffer, CB_BYTES, &offset);
        if (remap == 0) {
            psglFreeCommandBuffer(buffer);
            final_error = glGetError();
            final_mapped = mapped(buffer);
        }
    }
    printf("PSGL_TEARDOWN_UNMAP_FAIL own_unmap=%x error=0x%x again=0x%x remap=%x final=0x%x mapped=%d timeouts=%u\n",
           (unsigned)own_unmap, (unsigned)error, (unsigned)again, (unsigned)remap,
           (unsigned)final_error, final_mapped, timeouts() - before);
    check(kept, "a failed unmap reports GL_INVALID_OPERATION and keeps the buffer");
    check(kept && again == GL_INVALID_OPERATION,
          "the kept buffer stays registered (not GL_INVALID_VALUE)");
    check(remap == 0, "the kept buffer can be mapped again");
    check(final_error == GL_NO_ERROR, "freeing it once it can be unmapped succeeds");
    check(!final_mapped, "that free unmaps it");
    check(&psgl_teardown_wait_timeouts != 0 && timeouts() == before,
          "no teardown timeout on the unmap-failure path");
}

#if TEARDOWN_EXIT_MODE >= 1
static sys_ppu_thread_t hook_thread;
static int hook_fired, hook_started;
#endif
#if TEARDOWN_EXIT_MODE >= 2
static void inject(unsigned stage)
{
    if (stage != (TEARDOWN_EXIT_MODE == 2 ? 1u : 2u)) return;
    psgl_teardown_test_hook = 0;
    hook_fired = 1;
    queue_gated(14u);
    hook_started = start_release(&hook_thread, (BOUND_MS + 600u) * 1000u);
}
#endif

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
    free_unmap_failure_keeps_buffer();

    /* The application's own Finish leaves reference value 1 behind. */
    cellGcmFinish(gCellGcmCurrentContext, 1u);
    unsigned before = timeouts();
#if TEARDOWN_EXIT_MODE == 0
    sys_ppu_thread_t thread;
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
#else
    unsigned shut_before = shutdowns();
#if TEARDOWN_EXIT_MODE == 1
    queue_gated(14u);
    hook_fired = 1;
    hook_started = start_release(&hook_thread, (BOUND_MS + 600u) * 1000u);
#else
    int has_hook = &psgl_teardown_test_hook != 0;
    check(has_hook, "the library has the teardown test hook");
    if (has_hook) psgl_teardown_test_hook = inject;
#endif
    uint64_t t0 = now_ms();
    psglExit();
    uint64_t elapsed = now_ms() - t0;
    unsigned counted = timeouts() - before;
    unsigned shut_first = shutdowns() - shut_before;
    uint32_t seen = *done;
#if TEARDOWN_EXIT_MODE >= 2
    check(hook_fired && hook_started, "the injection point was reached");
    /* The device was released (mode 3) or is in teardown (mode 2). */
    PSGLcontext *again = psglCreateContext();
    if (again) psglMakeCurrent(again, device);
    check(again && psglGetCurrentDevice() == NULL, "a torn-down device is not bound again");
#endif
    if (hook_started) join_release(hook_thread);
    int drained = wait_done(14u);
    uint32_t label_before = *(volatile uint32_t *)cellGcmGetLabelAddress(PSGL_LABEL);
    unsigned before_retry = timeouts();
    uint64_t t1 = now_ms();
    psglExit();
    uint64_t elapsed2 = now_ms() - t1;
    uint32_t label_after = *(volatile uint32_t *)cellGcmGetLabelAddress(PSGL_LABEL);
    unsigned shut_total = shutdowns() - shut_before;
    printf("PSGL_TEARDOWN_EXIT_TIMEOUT mode=%d elapsed_ms=%llu done=%u timeouts=%u shutdowns_first=%u "
           "second_ms=%llu label=%08x->%08x shutdowns=%u retry_timeouts=%u\n",
           TEARDOWN_EXIT_MODE, (unsigned long long)elapsed, (unsigned)seen, counted, shut_first,
           (unsigned long long)elapsed2, (unsigned)label_before, (unsigned)label_after,
           shut_total, timeouts() - before_retry);
    check(elapsed + 100u >= BOUND_MS, "psglExit waits up to its bound");
    check(seen != 14u, "the gated work was still pending at the bound");
    check(&psgl_teardown_wait_timeouts != 0 && counted == 1u, "psglExit counts one teardown timeout");
    check(&psgl_teardown_shutdowns != 0 && shut_first == 0u, "the timed-out psglExit does not shut down");
    check(drained, "the gated work completes after release");
    check(&psgl_teardown_shutdowns != 0 && shut_total == 1u, "the retried psglExit shuts down");
    check(label_after != label_before, "the retried psglExit waits on a fresh token");
    check(timeouts() == before_retry, "the retried psglExit does not time out");
#endif
    printf("PSGL_TEARDOWN_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
