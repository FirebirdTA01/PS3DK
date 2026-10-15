/* The creator-delay hook that the metalock-contested build splices into newlib's lock_internal.c (force-included). */
#ifndef METALOCK_CONTESTED_HOOK_H
#define METALOCK_CONTESTED_HOOK_H
void test_after_claim(void);
/* Defined here, not with -D: a function-like macro on the command line depends on the CMake generator escaping it,
   and a lost definition leaves lock_internal.c's empty default, so the creator never signals the claim. */
#define LOCK_INTERNAL_TEST_AFTER_CLAIM() test_after_claim()
#endif
