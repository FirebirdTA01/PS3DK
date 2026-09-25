#ifndef REMOVE_TEST_REENT_H
#define REMOVE_TEST_REENT_H
struct _reent { int _errno; };
int _unlink_r(struct _reent *, const char *);
int _remove_r(struct _reent *, const char *);
#endif
