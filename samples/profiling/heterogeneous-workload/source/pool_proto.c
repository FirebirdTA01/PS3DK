#include "pool_proto.h"

int pp_submit(pe_epochs *e, const pool_io *io, unsigned workers, uint32_t sequence)
{
    if (!sequence || sequence >= PE_SEQ_MASK)
        return -1;
    io->barrier(io->ctx); /* the slots are free before any word goes out */
    uint64_t t = io->now_us(io->ctx);
    for (unsigned i = 0; i < workers; ++i) {
        int rc = io->write_mb(io->ctx, i, pe_job_word(e, i, sequence, t, 1));
        if (rc) {
            io->log(io->ctx, "mailbox", rc);
            pe_clear(e, "a partial mailbox send");
            return -1;
        }
    }
    return 0;
}

void pp_done(pe_epochs *e, const pool_io *io, unsigned workers)
{
    io->barrier(io->ctx); /* acquire: every completion word was seen before the statuses are read */
    uint64_t t = io->now_us(io->ctx);
    for (unsigned i = 0; i < workers; ++i)
        pe_job_done(e, i, t, io->slot_status(io->ctx, i));
}

void pp_failed(pe_epochs *e, const char *why) { pe_clear(e, why); }

int pp_keepalive(pe_epochs *e, const pool_io *io, unsigned workers, uint32_t sequence)
{
    if (!sequence || sequence >= PE_SEQ_MASK)
        return -1;
    uint64_t t = io->now_us(io->ctx);
    int sent[PE_MAX_WORKERS] = {0}, any = 0;
    for (unsigned i = 0; i < workers && i < PE_MAX_WORKERS; ++i) {
        uint32_t w = pe_job_word(e, i, sequence, t, 0);
        if (!w)
            continue;
        io->barrier(io->ctx);
        int rc = io->write_mb(io->ctx, i, w);
        if (rc) {
            io->log(io->ctx, "keepalive-mailbox", rc);
            pe_clear(e, "a partial mailbox send");
            return -1;
        }
        sent[i] = 1;
        any = 1;
    }
    if (!any)
        return 0;
    for (;;) {
        int all = 1;
        for (unsigned i = 0; i < workers && i < PE_MAX_WORKERS; ++i)
            if (sent[i] && io->slot_sequence(io->ctx, i) != sequence)
                all = 0;
        if (all)
            break;
        if (io->now_us(io->ctx) - t >= PE_DEADLINE_US) {
            io->log(io->ctx, "keepalive-deadline", 0);
            pe_clear(e, "a reset-only word overran its deadline");
            return -1;
        }
        io->sleep_us(io->ctx, 100);
    }
    io->barrier(io->ctx);
    uint64_t d = io->now_us(io->ctx);
    for (unsigned i = 0; i < workers && i < PE_MAX_WORKERS; ++i)
        if (sent[i])
            pe_job_done(e, i, d, io->slot_status(io->ctx, i));
    return 0;
}

int pp_close(pe_epochs *e, const pool_io *io, unsigned workers, int running, int *join_ok)
{
    *join_ok = 0;
    int markers = 0;
    if (running) {
        markers = 1;
        uint64_t t = io->now_us(io->ctx);
        for (unsigned i = 0; i < workers; ++i) {
            if (io->write_mb(io->ctx, i, 0)) {
                markers = 0;
                io->log(io->ctx, "exit-mailbox", (int)i);
            }
        }
        for (unsigned i = 0; markers && i < workers; ++i) {
            while (io->slot_sequence(io->ctx, i) != PE_SEQ_MASK) {
                if (io->now_us(io->ctx) - t >= PE_DEADLINE_US) {
                    markers = 0;
                    break;
                }
                io->sleep_us(io->ctx, 100);
            }
        }
        if (markers) {
            io->barrier(io->ctx); /* acquire: the markers before the final statuses */
            uint64_t d = io->now_us(io->ctx);
            for (unsigned i = 0; i < workers; ++i)
                pe_final(e, i, d, io->slot_status(io->ctx, i));
            /* every worker published its final marker and exits on its own: join, no terminate */
            int rc = io->join(io->ctx);
            if (rc) {
                io->log(io->ctx, "join", rc);
                pe_clear(e, "the SPU group join failed");
                markers = 0;
            } else {
                *join_ok = 1;
            }
        } else {
            io->log(io->ctx, "final-markers-missing", 0);
            pe_clear(e, "a final marker was missing");
        }
    }
    int rc = io->close_terminate(io->ctx);
    if (rc) {
        *join_ok = 0;
        pe_clear(e, "the SPU group close failed");
    }
    return rc;
}
