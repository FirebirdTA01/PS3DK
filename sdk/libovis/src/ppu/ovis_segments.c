/* cellOvisInvalidateOverlappedSegments / cellOvisFixSpuSegments - keep the
 * overlay sections out of an SPU image's initial load. */
#include <stdint.h>
#include <cell/ovis/util.h>

static int ovis_loads(const sys_spu_segment_t *s)
{
	return (s->type == SYS_SPU_SEGMENT_TYPE_COPY || s->type == SYS_SPU_SEGMENT_TYPE_FILL) &&
	       s->size > 0;
}

static int ovis_overlap(const sys_spu_segment_t *a, const sys_spu_segment_t *b)
{
	return a->ls_start < b->ls_start + (uint32_t)b->size &&
	       b->ls_start < a->ls_start + (uint32_t)a->size;
}

void cellOvisInvalidateOverlappedSegments(sys_spu_segment_t *segs, int *nsegs)
{
	int n, i, j, out = 0;
	if (!segs || !nsegs)
		return;
	n = *nsegs;
	for (i = 0; i < n; i++) {
		int drop = 0;
		if (ovis_loads(&segs[i]))
			for (j = 0; j < n && !drop; j++)
				drop = j != i && ovis_loads(&segs[j]) && ovis_overlap(&segs[i], &segs[j]);
		if (!drop)
			segs[out++] = segs[i];
	}
	*nsegs = out;
}

void cellOvisFixSpuSegments(sys_spu_image_t *image)
{
	if (image)
		cellOvisInvalidateOverlappedSegments(image->segs, &image->nsegs);
}
