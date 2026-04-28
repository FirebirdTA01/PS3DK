/* cell/spurs/job_queue_define.h - SPURS job-queue helper macros.
 *
 * Bracket macros for the cell::Spurs::JobQueue C++ namespace, a
 * conditional debug-print macro, and the early-return-on-error helper
 * the inline wrappers use.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_DEFINE_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_DEFINE_H__

#ifdef __cplusplus
#define __CELL_SPURS_JOBQUEUE_BEGIN \
namespace cell {                    \
namespace Spurs {                   \
namespace JobQueue {

#define __CELL_SPURS_JOBQUEUE_END   \
} /* namespace JobQueue */          \
} /* namespace Spurs    */          \
} /* namespace cell     */
#else
#define __CELL_SPURS_JOBQUEUE_BEGIN
#define __CELL_SPURS_JOBQUEUE_END
#endif

#ifndef __CELL_SPURS_FILELINE
#define __CELL_SPURS_STR(name)       #name
#define __CELL_SPURS_STR_LINE(line)  __CELL_SPURS_STR(line)
#define __CELL_SPURS_FILELINE        __FILE__ ":" __CELL_SPURS_STR_LINE(__LINE__) ": "
#endif

#ifdef _DEBUG
#  ifdef __SPU__
#    include <spu_printf.h>
#    define __CELL_SPURS_JOBQUEUE_DPRINTF__(...) spu_printf(__CELL_SPURS_FILELINE __VA_ARGS__)
#  else
#    include <stdio.h>
#    define __CELL_SPURS_JOBQUEUE_DPRINTF__(...) fprintf(stderr, __CELL_SPURS_FILELINE __VA_ARGS__)
#  endif
#else
#  define __CELL_SPURS_JOBQUEUE_DPRINTF__(...) ((void)0)
#endif

#undef __CELL_SPURS_RETURN_IF
#define __CELL_SPURS_RETURN_IF(exp)                                 \
    do {                                                            \
        int __ret = (exp);                                          \
        if (__builtin_expect(__ret, 0)) {                           \
            __CELL_SPURS_JOBQUEUE_DPRINTF__(" ret = %#x\n", __ret); \
            return __ret;                                           \
        }                                                           \
    } while (0)

/* Number of pre-defined job-descriptor sizes the descriptor pool
 * allocator partitions storage across (64/128/256/.../896 bytes). */
#define CELL_SPURS_JOBQUEUE_NUM_KIND_OF_SUPPORTED_JOBDESCRIPTOR 8

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_DEFINE_H__ */
