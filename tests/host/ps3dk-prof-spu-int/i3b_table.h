/* Test-only ABI between the harness and each independently compiled table implementation.
 * No private psp_* type crosses this boundary: the historical structs have different layouts. */
#ifndef I3B_TABLE_H
#define I3B_TABLE_H
#include <stddef.h>
#include <stdint.h>
typedef struct i3b_desc {
    uint32_t ls_addr, line;
    const char *name, *file;
} i3b_desc;
typedef struct i3b_stored_desc {
    uint32_t ls_addr, line;
    size_t name_offset, file_offset;
} i3b_stored_desc;
typedef struct i3b_table_result {
    int init_rc, rc;
    uint32_t index, nimages, build_len, ndesc;
    size_t arena_used, desc_offset, desc_bytes;
    uint8_t build_id[64];
    i3b_stored_desc descs[4096];
} i3b_table_result;
void i3b_table(const i3b_desc *, uint32_t, uint32_t, uint8_t *, size_t, i3b_table_result *);
void old_i3b_table(const i3b_desc *, uint32_t, uint32_t, uint8_t *, size_t, i3b_table_result *);
#endif
