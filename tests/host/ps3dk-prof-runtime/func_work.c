/* The instrumented workload of the function-path host test: compiled with -finstrument-functions and
 * -fno-omit-frame-pointer; the harness (func_test.c) and the library are not instrumented.  advance(n) moves the
 * fake timebase, so every elapsed value below is exact. */
#include <ps3dk/prof.h>
#include <setjmp.h>
#include <stdint.h>

#define W __attribute__((noinline, noipa))
void advance(uint64_t n);
void work_note(int what);

#define I static inline __attribute__((always_inline))
/* the review's counterexample: tree_b's work inside an instrumented always_inline leaf (same physical frame) */
I void tb_inl(void) { advance(10); }
W void tree_b(void) { tb_inl(); }
W void tree_c(void) { advance(100); }
W void tree_a(void) { advance(1); tree_b(); advance(2); tree_b(); tree_c(); advance(3); }
W void rec(int n) { advance(1); if (n > 1) rec(n - 1); }

/* depth: stack_depth 8 and 12 levels; each level costs 1 tick before descending */
W void deep(int n) { advance(1); if (n > 1) deep(n - 1); }
/* depth refusal through inline expansions: an inline leaf at every level, sharing that level's frame */
I void di_leaf(void) { }
W void deep_inl(int n) { advance(1); di_leaf(); if (n > 1) deep_inl(n - 1); }
static jmp_buf deep_jb;
W void deep_jump(int n, int top)
{
    if (n == top && setjmp(deep_jb))
        return; /* back in the outermost level after the jump */
    advance(1);
    if (n > 1)
        deep_jump(n - 1, top);
    else
        longjmp(deep_jb, 1);
}

/* overflow: node_capacity 4 = LOST + 3 nodes */
W void ov_g(void) { advance(7); }
W void ov_f1(void) { advance(1); }
W void ov_f2(void) { advance(2); }
W void ov_f3(void) { advance(3); }
W void ov_f4(void) { advance(4); ov_g(); }
W void ov_f5(void) { advance(5); }
W void ov_root(void) { ov_f1(); ov_f2(); ov_f3(); ov_f4(); ov_f5(); advance(1); }

/* longjmp out of a nested call, with a later call from the landing frame */
static jmp_buf lj_jb;
W void lj_deep(void) { advance(1); longjmp(lj_jb, 1); }
W void lj_mid(void) { advance(1); lj_deep(); }
W void lj_after(void) { advance(4); }
W void lj_outer(void)
{
    if (!setjmp(lj_jb))
        lj_mid();
    else
        lj_after();
    advance(1);
}
/* longjmp out of recursion to the outer activation of the same function, no call after landing */
static jmp_buf ra_jb;
W void ra(int n)
{
    if (n == 3 && setjmp(ra_jb))
        return;
    advance(1);
    if (n > 1)
        ra(n - 1);
    else
        longjmp(ra_jb, 1);
}
W void post_reset(void) { advance(2); }

/* a nested call spanning two cuts: the owner drains inside it, the leaf's entry takes each cut */
W void sp_leaf(void) { advance(1); }
W void sp_inner(void)
{
    advance(5);
    ps3dk_prof_drain();
    sp_leaf();
    advance(5);
    ps3dk_prof_drain();
    sp_leaf();
    advance(5);
}
W void sp_outer(void) { advance(5); sp_inner(); advance(5); }

/* threads */
W void w1(void) { advance(3); }
W void r_leaf(void) { advance(2); }
W void r_outer(void) { advance(1); r_leaf(); ps3dk_prof_thread_end(); advance(9); }
W void s_spin(void) { }
W void u_fn(void) { }
W void v_fn(void) { }
W void re_fn(void) { advance(1); }
W void reg_inner(void) { advance(6); }
W void reg_boot(void) { work_note(1); reg_inner(); advance(1); }

/* inline nesting and inline beside real calls at the same frame */
I void in_inner(void) { advance(3); }
I void in_outer(void) { advance(2); in_inner(); }
W void inl_nest(void) { in_outer(); }
W void real_leaf(void) { advance(4); }
I void im_a(void) { advance(1); }
I void im_b(void) { advance(1); real_leaf(); }
W void inl_mix(void) { im_a(); real_leaf(); im_b(); }

/* a nonlocal exit to a frame entered before registration: no retained activation matches, the stack resets */
void work_note(int what);
static jmp_buf k_jb;
W void k1(void) { advance(1); longjmp(k_jb, 1); }
W void boot3(void)
{
    if (!setjmp(k_jb)) {
        work_note(1);
        k1();
    }
    advance(1);
}

/* addresses of the inline functions for the symbol map (taken before init, so their hooks are not observed) */
W void *inline_addr(int k)
{
    switch (k) {
    case 0: return (void *)tb_inl;
    case 1: return (void *)di_leaf;
    case 2: return (void *)in_inner;
    case 3: return (void *)in_outer;
    case 4: return (void *)im_a;
    default: return (void *)im_b;
    }
}

/* the race and the cost probes */
W void rq(void) { }
W void rp(void) { rq(); rq(); }
W void leaf(void) { }
