/* Declares the creator-delay hook that the metalock-contested build splices into newlib's lock_internal.c. */
#ifndef METALOCK_CONTESTED_HOOK_H
#define METALOCK_CONTESTED_HOOK_H
void test_after_claim(void);
#endif
