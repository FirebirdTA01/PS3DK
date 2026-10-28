/* Compile-time ABI guard for <sys/dbg.h>: lv2 fills these records in
 * place, so every offset is part of the system interface.  Both data
 * models use the system's 32-bit layout; pointer members are 32-bit
 * effective addresses. */
#include <stddef.h>
#include <sys/dbg.h>

#ifdef __cplusplus
#define CHECK(c, m) static_assert(c, m)
#else
#define CHECK(c, m) _Static_assert(c, m)
#endif
#define SIZE(t, n) CHECK(sizeof(t) == (n), #t " size")
#define ALIGN(t, n) CHECK(__alignof__(t) == (n), #t " alignment")
#define OFFSET(t, f, n) CHECK(offsetof(t, f) == (n), #t "." #f " offset")
#define WIDTH(t, f, n) CHECK(sizeof(((t *)0)->f) == (n), #t "." #f " width")

/* The runner passes the data model it asked for; prove the compiler used it. */
CHECK(sizeof(void *) == EXPECTED_ABI_BYTES, "actual pointer data model");
CHECK(sizeof(long) == EXPECTED_ABI_BYTES, "actual long data model");

/* 128-bit register images. */
SIZE(sys_dbg_vr_t, 16);
ALIGN(sys_dbg_vr_t, 8);
WIDTH(sys_dbg_vr_t, byte, 16);
WIDTH(sys_dbg_vr_t, dw, 16);
SIZE(sys_dbg_spu_gpr_t, 16);
ALIGN(sys_dbg_spu_gpr_t, 8);
WIDTH(sys_dbg_spu_gpr_t, word, 16);
SIZE(sys_dbg_spu_fpscr_t, 16);
ALIGN(sys_dbg_spu_fpscr_t, 8);
WIDTH(sys_dbg_spu_fpscr_t, halfword, 16);

/* PPU thread context. */
SIZE(sys_dbg_ppu_thread_context_t, 1088);
ALIGN(sys_dbg_ppu_thread_context_t, 8);
OFFSET(sys_dbg_ppu_thread_context_t, gpr, 0);
OFFSET(sys_dbg_ppu_thread_context_t, cr, 256);
WIDTH(sys_dbg_ppu_thread_context_t, cr, 4);
OFFSET(sys_dbg_ppu_thread_context_t, xer, 264);
OFFSET(sys_dbg_ppu_thread_context_t, lr, 272);
OFFSET(sys_dbg_ppu_thread_context_t, ctr, 280);
OFFSET(sys_dbg_ppu_thread_context_t, pc, 288);
OFFSET(sys_dbg_ppu_thread_context_t, fpr, 296);
OFFSET(sys_dbg_ppu_thread_context_t, fpscr, 552);
WIDTH(sys_dbg_ppu_thread_context_t, fpscr, 4);
OFFSET(sys_dbg_ppu_thread_context_t, vr, 560);
OFFSET(sys_dbg_ppu_thread_context_t, vscr, 1072);

/* SPU thread contexts. */
SIZE(sys_dbg_spu_thread_context_t, 2872);
ALIGN(sys_dbg_spu_thread_context_t, 8);
OFFSET(sys_dbg_spu_thread_context_t, gpr, 0);
OFFSET(sys_dbg_spu_thread_context_t, npc, 2048);
OFFSET(sys_dbg_spu_thread_context_t, fpscr, 2052);
OFFSET(sys_dbg_spu_thread_context_t, srr0, 2056);
OFFSET(sys_dbg_spu_thread_context_t, spu_status, 2060);
OFFSET(sys_dbg_spu_thread_context_t, spu_cfg, 2064);
WIDTH(sys_dbg_spu_thread_context_t, spu_cfg, 8);
OFFSET(sys_dbg_spu_thread_context_t, mb_stat, 2072);
OFFSET(sys_dbg_spu_thread_context_t, ppu_mb, 2076);
OFFSET(sys_dbg_spu_thread_context_t, spu_mb, 2080);
WIDTH(sys_dbg_spu_thread_context_t, spu_mb, 16);
OFFSET(sys_dbg_spu_thread_context_t, decrementer, 2096);
OFFSET(sys_dbg_spu_thread_context_t, mfc_cq_sr, 2104);
WIDTH(sys_dbg_spu_thread_context_t, mfc_cq_sr, 768);

SIZE(sys_dbg_spu_thread_context2_t, 2888);
ALIGN(sys_dbg_spu_thread_context2_t, 8);
OFFSET(sys_dbg_spu_thread_context2_t, npc, 2048);
OFFSET(sys_dbg_spu_thread_context2_t, fpscr, 2056);
OFFSET(sys_dbg_spu_thread_context2_t, srr0, 2072);
OFFSET(sys_dbg_spu_thread_context2_t, spu_status, 2076);
OFFSET(sys_dbg_spu_thread_context2_t, spu_cfg, 2080);
OFFSET(sys_dbg_spu_thread_context2_t, mb_stat, 2088);
OFFSET(sys_dbg_spu_thread_context2_t, ppu_mb, 2092);
OFFSET(sys_dbg_spu_thread_context2_t, spu_mb, 2096);
OFFSET(sys_dbg_spu_thread_context2_t, decrementer, 2112);
OFFSET(sys_dbg_spu_thread_context2_t, mfc_cq_sr, 2120);

/* Synchronization-object information records. */
SIZE(sys_dbg_mutex_information_t, 72);
ALIGN(sys_dbg_mutex_information_t, 8);
OFFSET(sys_dbg_mutex_information_t, attr, 0);
OFFSET(sys_dbg_mutex_information_t, owner, 40);
OFFSET(sys_dbg_mutex_information_t, lock_counter, 48);
OFFSET(sys_dbg_mutex_information_t, cond_ref_counter, 52);
OFFSET(sys_dbg_mutex_information_t, cond_id, 56);
OFFSET(sys_dbg_mutex_information_t, wait_id_list, 60);
WIDTH(sys_dbg_mutex_information_t, wait_id_list, 4);
OFFSET(sys_dbg_mutex_information_t, wait_threads_num, 64);
OFFSET(sys_dbg_mutex_information_t, wait_all_threads_num, 68);

SIZE(sys_dbg_cond_information_t, 40);
ALIGN(sys_dbg_cond_information_t, 8);
OFFSET(sys_dbg_cond_information_t, attr, 0);
OFFSET(sys_dbg_cond_information_t, mutex_id, 24);
OFFSET(sys_dbg_cond_information_t, wait_id_list, 28);
WIDTH(sys_dbg_cond_information_t, wait_id_list, 4);
OFFSET(sys_dbg_cond_information_t, wait_threads_num, 32);
OFFSET(sys_dbg_cond_information_t, wait_all_threads_num, 36);

SIZE(sys_dbg_rwlock_information_t, 64);
ALIGN(sys_dbg_rwlock_information_t, 8);
OFFSET(sys_dbg_rwlock_information_t, attr, 0);
OFFSET(sys_dbg_rwlock_information_t, owner, 32);
OFFSET(sys_dbg_rwlock_information_t, r_wait_id_list, 40);
WIDTH(sys_dbg_rwlock_information_t, r_wait_id_list, 4);
OFFSET(sys_dbg_rwlock_information_t, r_wait_threads_num, 44);
OFFSET(sys_dbg_rwlock_information_t, r_wait_all_threads_num, 48);
OFFSET(sys_dbg_rwlock_information_t, w_wait_id_list, 52);
WIDTH(sys_dbg_rwlock_information_t, w_wait_id_list, 4);
OFFSET(sys_dbg_rwlock_information_t, w_wait_threads_num, 56);
OFFSET(sys_dbg_rwlock_information_t, w_wait_all_threads_num, 60);

SIZE(sys_dbg_event_queue_information_t, 56);
ALIGN(sys_dbg_event_queue_information_t, 8);
OFFSET(sys_dbg_event_queue_information_t, attr, 0);
OFFSET(sys_dbg_event_queue_information_t, event_queue_key, 16);
OFFSET(sys_dbg_event_queue_information_t, queue_size, 24);
OFFSET(sys_dbg_event_queue_information_t, wait_id_list, 28);
WIDTH(sys_dbg_event_queue_information_t, wait_id_list, 4);
OFFSET(sys_dbg_event_queue_information_t, wait_threads_num, 32);
OFFSET(sys_dbg_event_queue_information_t, wait_all_threads_num, 36);
OFFSET(sys_dbg_event_queue_information_t, equeue_list, 40);
WIDTH(sys_dbg_event_queue_information_t, equeue_list, 4);
OFFSET(sys_dbg_event_queue_information_t, readable_equeue_num, 44);
OFFSET(sys_dbg_event_queue_information_t, readable_all_equeue_num, 48);

SIZE(sys_dbg_semaphore_information_t, 56);
ALIGN(sys_dbg_semaphore_information_t, 8);
OFFSET(sys_dbg_semaphore_information_t, attr, 0);
OFFSET(sys_dbg_semaphore_information_t, max_val, 32);
OFFSET(sys_dbg_semaphore_information_t, cur_val, 36);
OFFSET(sys_dbg_semaphore_information_t, wait_id_list, 40);
WIDTH(sys_dbg_semaphore_information_t, wait_id_list, 4);
OFFSET(sys_dbg_semaphore_information_t, wait_threads_num, 44);
OFFSET(sys_dbg_semaphore_information_t, wait_all_threads_num, 48);

SIZE(sys_dbg_lwmutex_information_t, 40);
ALIGN(sys_dbg_lwmutex_information_t, 8);
OFFSET(sys_dbg_lwmutex_information_t, attr, 0);
OFFSET(sys_dbg_lwmutex_information_t, owner, 16);
OFFSET(sys_dbg_lwmutex_information_t, lock_counter, 24);
OFFSET(sys_dbg_lwmutex_information_t, wait_id_list, 28);
WIDTH(sys_dbg_lwmutex_information_t, wait_id_list, 4);
OFFSET(sys_dbg_lwmutex_information_t, wait_threads_num, 32);
OFFSET(sys_dbg_lwmutex_information_t, wait_all_threads_num, 36);

SIZE(sys_dbg_lwcond_information_t, 24);
ALIGN(sys_dbg_lwcond_information_t, 4);
OFFSET(sys_dbg_lwcond_information_t, attr, 0);
OFFSET(sys_dbg_lwcond_information_t, lwmutex, 8);
WIDTH(sys_dbg_lwcond_information_t, lwmutex, 4);
OFFSET(sys_dbg_lwcond_information_t, wait_id_list, 12);
WIDTH(sys_dbg_lwcond_information_t, wait_id_list, 4);
OFFSET(sys_dbg_lwcond_information_t, wait_threads_num, 16);
OFFSET(sys_dbg_lwcond_information_t, wait_all_threads_num, 20);

SIZE(sys_dbg_event_flag_wait_information_t, 16);
ALIGN(sys_dbg_event_flag_wait_information_t, 8);
OFFSET(sys_dbg_event_flag_wait_information_t, bitptn, 0);
OFFSET(sys_dbg_event_flag_wait_information_t, mode, 8);

SIZE(sys_dbg_event_flag_information_t, 56);
ALIGN(sys_dbg_event_flag_information_t, 8);
OFFSET(sys_dbg_event_flag_information_t, attr, 0);
OFFSET(sys_dbg_event_flag_information_t, cur_bitptn, 32);
OFFSET(sys_dbg_event_flag_information_t, wait_id_list, 40);
WIDTH(sys_dbg_event_flag_information_t, wait_id_list, 4);
OFFSET(sys_dbg_event_flag_information_t, wait_info_list, 44);
WIDTH(sys_dbg_event_flag_information_t, wait_info_list, 4);
OFFSET(sys_dbg_event_flag_information_t, wait_threads_num, 48);
OFFSET(sys_dbg_event_flag_information_t, wait_all_threads_num, 52);

/* Real objects, so every record is laid out, not only measured. */
sys_dbg_ppu_thread_context_t dbg_layout_ppu;
sys_dbg_spu_thread_context2_t dbg_layout_spu2;
sys_dbg_event_flag_information_t dbg_layout_flag;

unsigned int dbg_layout_read_fpscr(const sys_dbg_ppu_thread_context_t *c);
unsigned int dbg_layout_read_fpscr(const sys_dbg_ppu_thread_context_t *c)
{
    return c->fpscr;
}
