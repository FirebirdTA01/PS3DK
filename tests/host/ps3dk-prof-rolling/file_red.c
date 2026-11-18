/* Missing positioned adapter: test-only RED stand-in, never SDK-linked. */
#include "roll_file.h"
int pfr_file_bind(pfr_file *f, pfr_io *io, int fd, uint64_t extent)
{ (void)f; (void)io; (void)fd; (void)extent; return PFR_INVALID; }
