// t_d594ccd9 witness header: a well-formed guard defining a macro.  Before
// the fix the header was processed in a COPY of the preprocessor and its
// macro table was thrown away, so SAMPLER_T was unknown to the includer.
#ifndef CLOSED_GUARD_H
#define CLOSED_GUARD_H
#define SAMPLER_T sampler2D
#endif
