#ifndef __PS3DK_CELL_OVIS_H__
#define __PS3DK_CELL_OVIS_H__

#include <stdint.h>
#include <cellstatus.h>
#include <sys/spu_image.h>

#ifdef __cplusplus
extern "C" {
#endif

void cellOvisFixSpuSegments(sys_spu_image_t *image);
int cellOvisGetOverlayTableSize(const char *elf);
int cellOvisInitializeOverlayTable(void *ea_ovly_table, const char *elf);
void cellOvisInvalidateOverlappedSegments(sys_spu_segment_t *segs, int *nsegs);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_OVIS_H__ */
