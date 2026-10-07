#ifndef TLS_IE_H
#define TLS_IE_H

/* def.c defines the thread-locals and returns their addresses local-exec.
   ie.c (built twice, at -O2 and as ie-o0.c at -O0) reaches the same
   objects initial-exec, as an extern __thread reference in non-PIC code
   (GCC patch 0057).  Neither ie.c nor def.c includes a system header.  */
int *tls_ie_counter_le (void);
long long *tls_ie_wide_le (void);
int *tls_ie_array_le (void);

#define TLS_IE_DECLARE(p)						\
  int *p##counter_addr (void);						\
  int p##counter_get (void);						\
  void p##counter_set (int);						\
  long long p##wide_get (void);						\
  int *p##array_addr (void);						\
  int p##array_get (int);

TLS_IE_DECLARE (o2_)
TLS_IE_DECLARE (o0_)

#endif
