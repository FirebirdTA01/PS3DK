/* cell/ovis/util.h - fixing up an SPU image that carries overlays.
 *
 * An overlaid SPU ELF has one loadable segment per overlay section, and the
 * sections of one overlay region share their LS addresses.  Loading all of
 * them at thread start is wasted work (each overwrites the last) and none
 * is marked resident, so the SPU maps whichever it needs.  These drop the
 * overlapping segments from an image before the SPU thread is created. */
#ifndef PS3TC_CELL_OVIS_UTIL_H
#define PS3TC_CELL_OVIS_UTIL_H

#include <sys/spu_thread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Remove every COPY or FILL segment whose LS range overlaps another one;
 * the survivors keep their order and *nsegs becomes their count. */
void cellOvisInvalidateOverlappedSegments(sys_spu_segment_t *segs, int *nsegs);

/* cellOvisInvalidateOverlappedSegments over image->segs / image->nsegs. */
void cellOvisFixSpuSegments(sys_spu_image_t *image);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_OVIS_UTIL_H */
