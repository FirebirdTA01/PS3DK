/* libatomic.a: out-of-line cellAtomic* functions.
 *
 * <cell/atomic.h> defines every operation as a static inline.  Code that
 * takes an operation's address, or declares the function itself and links
 * -latomic, needs a real external definition.  The Makefile compiles this
 * file once per operation, so each archive member defines exactly one
 * function, with two definitions on the command line:
 *
 *   -DcellAtomic<Op>=__ps3dk_inline_cellAtomic<Op>
 *        renames that operation's inline while the header is included;
 *   -DCELL_ATOMIC_OP=<n>
 *        selects the external definition below, which forwards to it.
 *
 * Both forms therefore share the header's one body.
 */
#include <stdint.h>
#include <cell/atomic.h>

#ifndef CELL_ATOMIC_OP
#error "compile with -DCELL_ATOMIC_OP=<n> and the matching rename (see the Makefile)"
#endif

#define BINARY(op, type)                                                     \
    type cellAtomic##op(type *ls, uint64_t ea, type value)                   \
    {                                                                        \
        return __ps3dk_inline_cellAtomic##op(ls, ea, value);                 \
    }
#define UNARY(op, type)                                                      \
    type cellAtomic##op(type *ls, uint64_t ea)                               \
    {                                                                        \
        return __ps3dk_inline_cellAtomic##op(ls, ea);                        \
    }
#define CAS(op, type)                                                        \
    type cellAtomic##op(type *ls, uint64_t ea, type compare, type swap)      \
    {                                                                        \
        return __ps3dk_inline_cellAtomic##op(ls, ea, compare, swap);         \
    }

#undef cellAtomicAdd32
#undef cellAtomicSub32
#undef cellAtomicAnd32
#undef cellAtomicOr32
#undef cellAtomicStore32
#undef cellAtomicIncr32
#undef cellAtomicDecr32
#undef cellAtomicNop32
#undef cellAtomicTestAndDecr32
#undef cellAtomicCompareAndSwap32
#undef cellAtomicAdd64
#undef cellAtomicSub64
#undef cellAtomicAnd64
#undef cellAtomicOr64
#undef cellAtomicStore64
#undef cellAtomicIncr64
#undef cellAtomicDecr64
#undef cellAtomicNop64
#undef cellAtomicTestAndDecr64
#undef cellAtomicCompareAndSwap64

#if CELL_ATOMIC_OP == 1
BINARY(Add32, uint32_t)
#elif CELL_ATOMIC_OP == 2
BINARY(Sub32, uint32_t)
#elif CELL_ATOMIC_OP == 3
BINARY(And32, uint32_t)
#elif CELL_ATOMIC_OP == 4
BINARY(Or32, uint32_t)
#elif CELL_ATOMIC_OP == 5
BINARY(Store32, uint32_t)
#elif CELL_ATOMIC_OP == 6
UNARY(Incr32, uint32_t)
#elif CELL_ATOMIC_OP == 7
UNARY(Decr32, uint32_t)
#elif CELL_ATOMIC_OP == 8
UNARY(Nop32, uint32_t)
#elif CELL_ATOMIC_OP == 9
UNARY(TestAndDecr32, uint32_t)
#elif CELL_ATOMIC_OP == 10
CAS(CompareAndSwap32, uint32_t)
#elif CELL_ATOMIC_OP == 11
BINARY(Add64, uint64_t)
#elif CELL_ATOMIC_OP == 12
BINARY(Sub64, uint64_t)
#elif CELL_ATOMIC_OP == 13
BINARY(And64, uint64_t)
#elif CELL_ATOMIC_OP == 14
BINARY(Or64, uint64_t)
#elif CELL_ATOMIC_OP == 15
BINARY(Store64, uint64_t)
#elif CELL_ATOMIC_OP == 16
UNARY(Incr64, uint64_t)
#elif CELL_ATOMIC_OP == 17
UNARY(Decr64, uint64_t)
#elif CELL_ATOMIC_OP == 18
UNARY(Nop64, uint64_t)
#elif CELL_ATOMIC_OP == 19
UNARY(TestAndDecr64, uint64_t)
#elif CELL_ATOMIC_OP == 20
CAS(CompareAndSwap64, uint64_t)
#else
#error "unknown CELL_ATOMIC_OP"
#endif
