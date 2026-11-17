/* Explicit missing-feature stand-in for the pre-implementation RED run only.
 * It supplies no rolling implementation. Never compile this into production. */
#include "contract.h"
size_t pfr_state_bytes(void) { return 1; }
int pfr_plan(const pfr_config *c, pfr_layout *l)
{ (void)c; (void)l; return PFR_FAILED; }
int pfr_format(pfr_store *s, const pfr_io *i, const pfr_config *c, void *b, size_t n)
{ (void)s; (void)i; (void)c; (void)b; (void)n; return PFR_FAILED; }
int pfr_open(pfr_store *s, const pfr_io *i, const pfr_config *c, void *b, size_t n)
{ (void)s; (void)i; (void)c; (void)b; (void)n; return PFR_FAILED; }
int pfr_commit(pfr_store *s, const void *c, size_t n, const void *p, size_t k)
{ (void)s; (void)c; (void)n; (void)p; (void)k; return PFR_FAILED; }
int pfr_get_view(const pfr_store *s, pfr_view *v)
{ (void)s; (void)v; return PFR_FAILED; }
int pfr_get_segment(pfr_store *s, uint32_t i, pfr_segment *v)
{ (void)s; (void)i; (void)v; return PFR_FAILED; }
