#include "roll_output.h"
size_t pfo_state_bytes(void) { return 8; }
int pfo_bind(pfo_output *o,pfr_store *s,const pfo_buffers *b,const void *p,size_t n)
{ (void)o;(void)s;(void)b;(void)p;(void)n;return PFR_INVALID; }
int pfo_accept(pfo_output *o,const void *p,size_t n)
{ (void)o;(void)p;(void)n;return PFR_INVALID; }
int pfo_flush(pfo_output *o) { (void)o;return PFR_INVALID; }
int pfo_finish(pfo_output *o) { (void)o;return PFR_INVALID; }
int pfo_get_view(const pfo_output *o,pfo_view *v)
{ (void)o;(void)v;return PFR_INVALID; }
