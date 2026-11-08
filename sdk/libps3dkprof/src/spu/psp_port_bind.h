/* How the producer core reaches its port (design I3 r2 D1).  Included by psp_spu.c only.
 *   host (default): the port table, exactly the calls the host model always made (the preprocessed core is
 *                   token-identical to the table-only source);
 *   PSP_PORT_HOSTRAW (host tests): the table, but each atomic-status read goes through the SPU translation, so the
 *                   operation named at every call site is exercised against raw MFC values;
 *   __SPU__:        static inline MFC channel operations (psp_port_spu.h); no table, no ctx.
 * PSP_AS_READ names ONE operation per call site. */
#ifndef PSP_PORT_BIND_H
#define PSP_PORT_BIND_H

#if defined(__SPU__)

#include "psp_port_spu.h"

#define PSP_PORT_DECL (void)s;
#define PSP_P0(fn) psp_port_spu_##fn()
#define PSP_P(fn, ...) psp_port_spu_##fn(__VA_ARGS__)
#define PSP_S0(fn) psp_port_spu_##fn()
#define PSP_S(fn, ...) psp_port_spu_##fn(__VA_ARGS__)
#define PSP_AS_READ(op) psp_as_translate((op), psp_port_spu_rd_atomic_raw())

#else

#define PSP_PORT_DECL const psp_port *p = s->port;
#define PSP_P0(fn) p->fn(p->ctx)
#define PSP_P(fn, ...) p->fn(p->ctx, __VA_ARGS__)
#define PSP_S0(fn) s->port->fn(s->port->ctx)
#define PSP_S(fn, ...) s->port->fn(s->port->ctx, __VA_ARGS__)
#if defined(PSP_PORT_HOSTRAW)
#include "psp_as.h"
#define PSP_AS_READ(op) psp_as_translate((op), p->rd_atomic_stat(p->ctx))
#else
#define PSP_AS_READ(op) p->rd_atomic_stat(p->ctx)
#endif

#endif

#endif
