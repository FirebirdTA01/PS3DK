#include "worker_proto.h"

/* flush, then write the decrementer; EAGAIN (earlier events not all published) flushes and retries, bounded.  Any
 * other failure is never retried (the write may have happened). */
static int reset_dec(const worker_io *io)
{
    for (int k = 0; k < WP_RESET_TRIES; ++k) {
        if (io->flush(io->ctx) != WP_OK)
            return -1;
        int rc = io->dec_write(io->ctx);
        if (rc == WP_OK)
            return 0;
        if (rc != WP_EAGAIN)
            return -1;
    }
    return -1;
}

uint32_t worker_run(const worker_io *io, int profiled)
{
    uint32_t status = 0;
    int on = 0, attached = 0;
    if (profiled) {
        if (io->attach(io->ctx) != WP_OK) {
            status |= PE_SPU_ATTACH_FAILED;
        } else {
            attached = 1;
            if (reset_dec(io) != 0)
                status |= PE_SPU_RESET_FAILED;
            else
                on = 1;
        }
    }
    for (;;) {
        uint32_t word = io->read_mbox(io->ctx);
        if (!word)
            break;
        uint32_t sequence = word & PE_SEQ_MASK;
        if (on && (word & (PE_FLAG_RESET | PE_FLAG_RESET_ONLY)) && reset_dec(io) != 0) {
            status |= PE_SPU_RESET_FAILED;
            on = 0;
        }
        if (word & PE_FLAG_RESET_ONLY) {
            io->no_work(io->ctx);
        } else if (on) {
            uint32_t z = io->zone_begin(io->ctx);
            io->work(io->ctx);
            io->zone_end(io->ctx, z);
        } else {
            io->work(io->ctx);
        }
        if (on && io->flush(io->ctx) != WP_OK) {
            status |= PE_SPU_FLUSH_FAILED;
            on = 0;
        }
        io->publish(io->ctx, status, sequence);
    }
    if (attached && io->fini(io->ctx) != WP_OK)
        status |= PE_SPU_FINI_FAILED;
    io->publish(io->ctx, status, PE_SEQ_MASK); /* the final marker: nothing is written after it */
    return status;
}
