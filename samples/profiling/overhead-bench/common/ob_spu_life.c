#include "ob_spu_life.h"

#include <string.h>

static void lg(ob_spu *s, const char *what, int rc)
{
    if (s->p->log)
        s->p->log(s->p->ctx, what, rc);
}

/* finite: at most s->polls polls; 0 and the words when reply number `expect` arrived */
static int wait_reply(ob_spu *s, uint32_t w[16])
{
    ++s->expect;
    for (uint32_t i = 0; i < s->polls; ++i) {
        uint32_t q = s->p->reply_seq(s->p->ctx);
        if (q == s->expect) {
            s->p->reply_words(s->p->ctx, w);
            return 0;
        }
        if (q != s->expect - 1u) { /* neither the previous nor this reply: identity lost */
            lg(s, "spu: reply sequence out of step", (int)q);
            return -1;
        }
        s->p->pause(s->p->ctx);
    }
    lg(s, "spu: no reply within the poll bound", (int)s->expect);
    return -1;
}

int ob_spu_open(ob_spu *s, const ob_spu_port *p, uint64_t reply_ea, uint32_t polls)
{
    uint32_t image = 0, w[16];
    int rc;
    memset(s, 0, sizeof *s);
    s->p = p;
    s->reply_ea = reply_ea;
    s->polls = polls;
    if ((rc = p->spu_init(p->ctx)) != 0) {
        lg(s, "spu: initialize failed", rc);
        return -1;
    }
    if ((rc = p->image_import(p->ctx)) != 0) {
        lg(s, "spu: image import failed", rc);
        return -1;
    }
    s->image = 1;
    if ((rc = p->image_register(p->ctx, &image)) != 0) {
        lg(s, "spu: image register (profiler) failed", rc);
        return -1;
    }
    if ((rc = p->lane_create(p->ctx, image, &s->lane)) != 0) {
        lg(s, "spu: lane create failed", rc);
        return -1;
    }
    s->lane_owned = 1;
    if ((rc = p->group_create(p->ctx)) != 0) {
        lg(s, "spu: group create failed", rc);
        return -1;
    }
    s->group = 1;
    if ((rc = p->thread_init(p->ctx, &s->lane, reply_ea)) != 0) {
        lg(s, "spu: thread initialize failed", rc);
        return -1;
    }
    s->thread = 1;
    rc = p->group_start(p->ctx);
    if (rc == OB_SPU_START_NOT_STARTED) {
        lg(s, "spu: group start failed, proven non-start", rc);
        return -1; /* nothing runs: close unwinds without terminate/join */
    }
    s->started = 1; /* 0, or an unclassified failure: the producer may run */
    if (rc != 0) {
        s->start_uncertain = 1;
        lg(s, "spu: group start failed, UNCERTAIN: settled by terminate + join before any release", rc);
        return -1; /* not alive: close terminates (checked) and joins (checked) */
    }
    if (wait_reply(s, w) != 0)
        return -1; /* not alive: close terminates */
    if (w[8] != 0) {
        lg(s, "spu: the startup reply carries a command word (identity)", (int)w[8]);
        return -1; /* not alive: close terminates */
    }
    s->alive = 1;
    s->dec_reset = w[9];
    if (w[0] != 0 || w[1] != 0 || w[9] != 1) {
        lg(s, "spu: startup acknowledgement: attach or decrementer write failed", (int)(w[0] ? w[0] : w[1]));
        return -1; /* alive: close sends the exit command */
    }
    return 0;
}

int ob_spu_block(ob_spu *s, uint32_t c, uint32_t tu, ob_spu_reply *out)
{
    uint32_t w[16];
    memset(out, 0, sizeof *out);
    if (!s->alive) {
        out->rc = OB_RC_DEAD;
        return -1;
    }
    s->cmd_seq = (s->cmd_seq + 1u) & 0xFFFFu;
    if (s->cmd_seq == 0)
        s->cmd_seq = 1; /* the word is never 0 (0 is the exit command) */
    out->cmd = OB_SPU_CMD(s->cmd_seq, c, tu);
    out->ppu_t0 = s->p->tb(s->p->ctx);
    if (s->p->write_mb(s->p->ctx, out->cmd) != 0) {
        s->alive = 0;
        out->rc = OB_RC_SEND;
        lg(s, "spu: command send failed", (int)out->cmd);
        return -1;
    }
    if (wait_reply(s, w) != 0) {
        s->alive = 0;
        out->rc = OB_RC_NOREPLY;
        return -1;
    }
    out->ppu_t1 = s->p->tb(s->p->ctx);
    memcpy(out->w, w, sizeof out->w);
    out->echo = w[8];
    out->dec_reset = w[9];
    if (out->echo != out->cmd) {
        s->alive = 0;
        out->rc = OB_RC_IDENTITY;
        lg(s, "spu: reply identity differs from the command", (int)out->echo);
        return -1;
    }
    return 0;
}

int ob_spu_close(ob_spu *s)
{
    const ob_spu_port *p = s->p;
    int failed = 0, rc;
    uint32_t w[16];
    if (!p)
        return OB_LIFE_FAILED;
    if (s->started) {
        if (s->alive) {
            if ((rc = p->write_mb(p->ctx, OB_SPU_EXIT)) != 0) {
                lg(s, "spu: exit command send failed", rc);
                failed = 1;
            } else if (wait_reply(s, w) != 0) {
                failed = 1;
            } else if (w[8] != OB_SPU_EXIT) {
                lg(s, "spu: the exit reply carries another command (identity): exit not proven", (int)w[8]);
                failed = 1;
            } else {
                s->exited = 1;
                if (w[0] != 0) {
                    lg(s, "spu: writer fini failed", (int)w[0]);
                    failed = 1;
                }
            }
            s->alive = 0;
        }
        if (!s->exited) {
            failed = 1;
            if ((rc = p->terminate(p->ctx)) != 0) {
                lg(s, "spu: terminate failed: the SPU may still run; everything retained", rc);
                s->uncertain = 1;
                return OB_LIFE_RETAINED;
            }
        }
        if ((rc = p->join(p->ctx)) != 0) {
            lg(s, "spu: group join failed: the SPU may still run; everything retained", rc);
            s->uncertain = 1;
            return OB_LIFE_RETAINED;
        }
        s->joined = 1;
    }
    /* here no producer runs: the group never started, or its join succeeded */
    int retained = 0;
    if (s->lane_owned) {
        if ((rc = p->lane_end(p->ctx, s->lane.handle)) != 0) {
            lg(s, "spu: lane_end failed: the lane stays charged (retained)", rc);
            retained = 1;
        } else {
            s->lane_owned = 0;
        }
    }
    if (s->group) {
        if ((rc = p->destroy(p->ctx)) != 0) {
            lg(s, "spu: group destroy failed", rc);
            failed = 1;
        } else {
            s->group = 0;
        }
    }
    if (s->image) {
        if ((rc = p->image_close(p->ctx)) != 0) {
            lg(s, "spu: image close failed", rc);
            failed = 1;
        } else {
            s->image = 0;
        }
    }
    if (retained)
        return OB_LIFE_RETAINED;
    return failed ? OB_LIFE_FAILED : OB_LIFE_CLEAN;
}
