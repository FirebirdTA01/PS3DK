/*
 * Shared pieces of the sample-trio fault witnesses.
 *
 * Each witness compiles one sample's main.c with some of the calls it
 * makes replaced by local fakes, renames the sample's main to
 * sample_main, and runs it once for the case selected by WITNESS_CASE.
 * The witness then compares the sample's exit status and the calls the
 * fakes saw against what the case requires, and prints exactly one of
 *   SAMPLE_WITNESS_OK case=<name>
 *   SAMPLE_WITNESS_FAIL case=<name> <reason>
 */
#ifndef SAMPLE_TRIO_WITNESS_H
#define SAMPLE_TRIO_WITNESS_H

#include <stdio.h>

#ifndef WITNESS_CASE
#error "WITNESS_CASE must be defined"
#endif
#ifndef WITNESS_NAME
#error "WITNESS_NAME must be defined"
#endif

static int witness_bad;

#define WITNESS_REQUIRE(cond)                                            \
    do {                                                                 \
        if (!(cond)) {                                                   \
            witness_bad++;                                               \
            printf("SAMPLE_WITNESS_FAIL case=%s requirement %s\n",       \
                   WITNESS_NAME, #cond);                                 \
        }                                                                \
    } while (0)

static int witness_finish(void)
{
    if (!witness_bad)
        printf("SAMPLE_WITNESS_OK case=%s\n", WITNESS_NAME);
    return 0;
}

#endif
