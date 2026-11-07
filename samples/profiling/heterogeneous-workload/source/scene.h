#ifndef HETEROGENEOUS_SCENE_H
#define HETEROGENEOUS_SCENE_H
#include <stdint.h>
uint64_t sample_now_us(void);
int scene_init(void);
int scene_poll(void); /* 0 continue, 1 requested exit, -1 failure */
int scene_render(int heavy,const char *phase,unsigned workers,uint64_t compute_us,
                 uint64_t frame_us,uint64_t *gpu_begin,uint64_t *gpu_end,int *gpu_valid);
void scene_close(void);
void scene_set_status(const char *text); /* one extra screen line; "" for none */
/* Plan A r2 A-C3: timebase boundary readings (overhead-bench/common/ob_timer.h) the last scene_render took:
 * submit = the command build up to the flush, flip wait = the wait for the flip and the completion label */
typedef struct scene_tb {
    uint64_t submit_begin, submit_end, flipwait_begin, flipwait_end;
} scene_tb;
extern scene_tb g_scene_tb;
#endif
