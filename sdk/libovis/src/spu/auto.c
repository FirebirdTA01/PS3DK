/* State for automatic mapping (<cell/ovis/auto.h>), and the trace switch. */
#include <stdint.h>
#include <cell/ovis.h>

unsigned int gCellOvisTag;
uint64_t gCellOvisTable;

void cellOvisEnableSpursTrace(int enable)
{
	(void)enable;
}
