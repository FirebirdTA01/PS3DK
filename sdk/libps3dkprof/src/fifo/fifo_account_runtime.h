/* Private additive seam. Sole registered GCM owner, immutable IO offsets;
 * scalar accounting takes no lock, allocation, sink or SDK call. The separate
 * core owner-window query copies identity under its existing window lock and
 * releases that lock before any GCM observation. Enable only after native prepare,
 * PUT, wrap, reset and frame/window boundaries are wired. A report serial is
 * NOT a published IO offset. Existing AUTO pre-wrap hooks are independent. */
#ifndef PS3DK_PROF_FIFO_ACCOUNT_RUNTIME_H
#define PS3DK_PROF_FIFO_ACCOUNT_RUNTIME_H
#include <stdint.h>
int ps3dk_prof__fifo_account_start(void *ctx, uint32_t current, uint32_t published);
void ps3dk_prof__fifo_account_prepare(void *ctx, uint32_t current);
void ps3dk_prof__fifo_account_publish(void *ctx, uint32_t published);
void ps3dk_prof__fifo_account_begin(void *ctx, uint32_t owner, uint64_t frame, uint64_t window, uint32_t current);
void ps3dk_prof__fifo_account_end(void *ctx, uint32_t owner, uint64_t frame, uint64_t window, uint32_t current);
void ps3dk_prof__fifo_account_unknown(void *ctx);
#ifdef PS3DK_PROF_WITH_AUTO
#define PS3DK_PROF_FIFO_NATIVE_ACCOUNT 1
#define PS3DK_PROF_FIFO_END_KNOWN 1
int ps3dk_prof__auto_fifo_end_known(void *ctx, const void *registered_physical_end, const void *visible_end);
void ps3dk_prof__fifo_account_native_start(void *ctx);
void ps3dk_prof__fifo_account_native_frame(uint64_t frame, uint64_t previous_window, uint64_t window);
void ps3dk_prof__fifo_account_native_end(void *ctx, uint32_t owner, uint64_t frame, uint64_t window);
void ps3dk_prof__fifo_account_native_publish(void *ctx, uint32_t offset);
void ps3dk_prof__fifo_account_check_context(void *ctx);
uint64_t ps3dk_prof__fifo_owner_window(void);
#endif
#endif
