/*
 * spu-thread-events-test.c — Static assertion and signature verification test
 * for SPU thread event constants and syscall wrappers.
 */

#include <stdint.h>
#include <sys/spu_thread.h>

#if defined(__cplusplus)
static_assert(SYS_SPU_THREAD_EVENT_USER == 0x1, "SYS_SPU_THREAD_EVENT_USER value mismatch");
static_assert(SYS_SPU_THREAD_EVENT_DMA == 0x2, "SYS_SPU_THREAD_EVENT_DMA value mismatch");
static_assert(SYS_SPU_THREAD_EVENT_USER_KEY == 0xFFFFFFFF53505501ULL, "SYS_SPU_THREAD_EVENT_USER_KEY value mismatch");
static_assert(SYS_SPU_THREAD_EVENT_DMA_KEY == 0xFFFFFFFF53505502ULL, "SYS_SPU_THREAD_EVENT_DMA_KEY value mismatch");
static_assert(SYS_SPU_THREAD_DMA_COMPLETION_STOP == 0x0U, "SYS_SPU_THREAD_DMA_COMPLETION_STOP value mismatch");
static_assert(SYS_SPU_THREAD_DMA_COMPLETION_ANY == 0x1U, "SYS_SPU_THREAD_DMA_COMPLETION_ANY value mismatch");
static_assert(SYS_SPU_THREAD_DMA_COMPLETION_ALL == 0x2U, "SYS_SPU_THREAD_DMA_COMPLETION_ALL value mismatch");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(SYS_SPU_THREAD_EVENT_USER == 0x1, "SYS_SPU_THREAD_EVENT_USER value mismatch");
_Static_assert(SYS_SPU_THREAD_EVENT_DMA == 0x2, "SYS_SPU_THREAD_EVENT_DMA value mismatch");
_Static_assert(SYS_SPU_THREAD_EVENT_USER_KEY == 0xFFFFFFFF53505501ULL, "SYS_SPU_THREAD_EVENT_USER_KEY value mismatch");
_Static_assert(SYS_SPU_THREAD_EVENT_DMA_KEY == 0xFFFFFFFF53505502ULL, "SYS_SPU_THREAD_EVENT_DMA_KEY value mismatch");
_Static_assert(SYS_SPU_THREAD_DMA_COMPLETION_STOP == 0x0U, "SYS_SPU_THREAD_DMA_COMPLETION_STOP value mismatch");
_Static_assert(SYS_SPU_THREAD_DMA_COMPLETION_ANY == 0x1U, "SYS_SPU_THREAD_DMA_COMPLETION_ANY value mismatch");
_Static_assert(SYS_SPU_THREAD_DMA_COMPLETION_ALL == 0x2U, "SYS_SPU_THREAD_DMA_COMPLETION_ALL value mismatch");
#else
typedef char assert_event_user[(SYS_SPU_THREAD_EVENT_USER == 0x1) ? 1 : -1];
typedef char assert_event_dma[(SYS_SPU_THREAD_EVENT_DMA == 0x2) ? 1 : -1];
typedef char assert_event_user_key[(SYS_SPU_THREAD_EVENT_USER_KEY == 0xFFFFFFFF53505501ULL) ? 1 : -1];
typedef char assert_event_dma_key[(SYS_SPU_THREAD_EVENT_DMA_KEY == 0xFFFFFFFF53505502ULL) ? 1 : -1];
typedef char assert_dma_stop[(SYS_SPU_THREAD_DMA_COMPLETION_STOP == 0x0U) ? 1 : -1];
typedef char assert_dma_any[(SYS_SPU_THREAD_DMA_COMPLETION_ANY == 0x1U) ? 1 : -1];
typedef char assert_dma_all[(SYS_SPU_THREAD_DMA_COMPLETION_ALL == 0x2U) ? 1 : -1];
#endif

int test_function_signatures(void)
{
    int (*fn_connect)(sys_spu_thread_t, sys_event_queue_t, sys_event_type_t, uint8_t) = sys_spu_thread_connect_event;
    int (*fn_disconnect)(sys_spu_thread_t, sys_event_type_t, uint8_t) = sys_spu_thread_disconnect_event;
    int (*fn_bind)(sys_spu_thread_t, sys_event_queue_t, uint32_t) = sys_spu_thread_bind_queue;
    int (*fn_unbind)(sys_spu_thread_t, uint32_t) = sys_spu_thread_unbind_queue;

    s32 (*fn_psl1ght_connect)(sys_spu_thread_t, sys_event_queue_t, u32, u8) = sysSpuThreadConnectEvent;
    s32 (*fn_psl1ght_disconnect)(sys_spu_thread_t, u32, u8) = sysSpuThreadDisconnectEvent;
    s32 (*fn_psl1ght_bind)(sys_spu_thread_t, sys_event_queue_t, u32) = sysSpuThreadBindQueue;
    s32 (*fn_psl1ght_unbind)(sys_spu_thread_t, u32) = sysSpuThreadUnbindQueue;

    (void)fn_connect;
    (void)fn_disconnect;
    (void)fn_bind;
    (void)fn_unbind;
    (void)fn_psl1ght_connect;
    (void)fn_psl1ght_disconnect;
    (void)fn_psl1ght_bind;
    (void)fn_psl1ght_unbind;
    return 0;
}

int main(void)
{
    return test_function_signatures();
}
