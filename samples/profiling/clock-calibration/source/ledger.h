/* The probe's SPU resource ledger, portable so the host tests can inject every failure.
 *
 * Teardown order, each step only after the previous one succeeded: a started group is terminated, then joined; only
 * a joined (or never-started) group is destroyed; the image is closed only after its group is destroyed. A failed
 * step stops the teardown there: nothing is reused or destroyed after a failed terminate or join. The first failure
 * of the run and the first cleanup failure are kept separately. Note: none of these LV2 calls has a deadline. */
#ifndef CLOCKCAL_LEDGER_H
#define CLOCKCAL_LEDGER_H

typedef struct {
    int (*terminate)(void *ctx);
    int (*join)(void *ctx);
    int (*destroy)(void *ctx);
    int (*image_close)(void *ctx);
    void *ctx;
} ledger_ops;

typedef struct {
    int image_open, group_created, group_started;
    int cleanup_rc;         /* the first cleanup failure (0: none) */
    const char *cleanup_at; /* which step failed first, or "" */
    int left_running;       /* 1: the group could not be stopped; resources were left as they are */
} ledger;

static inline void ledger_teardown(ledger *l, const ledger_ops *ops)
{
    int rc;
    l->cleanup_rc = 0;
    l->cleanup_at = "";
    l->left_running = 0;
    if (l->group_started) {
        if ((rc = ops->terminate(ops->ctx)) != 0) {
            l->cleanup_rc = rc, l->cleanup_at = "terminate", l->left_running = 1;
            return;
        }
        if ((rc = ops->join(ops->ctx)) != 0) {
            l->cleanup_rc = rc, l->cleanup_at = "join", l->left_running = 1;
            return;
        }
        l->group_started = 0;
    }
    if (l->group_created) {
        if ((rc = ops->destroy(ops->ctx)) != 0) {
            l->cleanup_rc = rc, l->cleanup_at = "destroy";
            return;
        }
        l->group_created = 0;
    }
    if (l->image_open) {
        if ((rc = ops->image_close(ops->ctx)) != 0) {
            l->cleanup_rc = rc, l->cleanup_at = "image-close";
            return;
        }
        l->image_open = 0;
    }
}

#endif
