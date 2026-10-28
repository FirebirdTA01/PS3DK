/*
 * cellfs-struct-layout-test.c - the records <cell/fs/cell_fs_file_api.h>
 * hands to the file-system PRX must have the system's layout in both PPU
 * data models: lv2 reads them by byte offset.
 *
 * The directory/status records are 4-byte packed; the stream ring-buffer
 * description and the AIO request keep natural alignment.  An AIO request
 * with a packed layout put `buf` where lv2 reads the high word of `size`,
 * and every cellFsAioRead completed with CELL_FS_EFAULT on hardware.
 *
 * Compiled (not run) by cellfs-struct-layout-test.sh; every assert is a
 * compile-time check.
 */
#include <stddef.h>
#include <cell/fs/cell_fs_file_api.h>

#if defined(__cplusplus)
#define LAYOUT_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define LAYOUT_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define SIZE(T, n)      LAYOUT_ASSERT(sizeof(T) == (n), #T " size")
#define ALIGN(T, n)     LAYOUT_ASSERT(__alignof__(T) == (n), #T " alignment")
#define AT(T, f, n)     LAYOUT_ASSERT(offsetof(T, f) == (n), #T "." #f " offset")

SIZE(CellFsDirent, 258);
ALIGN(CellFsDirent, 1);
AT(CellFsDirent, d_type, 0);
AT(CellFsDirent, d_namlen, 1);
AT(CellFsDirent, d_name, 2);

SIZE(CellFsStat, 52);
ALIGN(CellFsStat, 4);
AT(CellFsStat, st_mode, 0);
AT(CellFsStat, st_uid, 4);
AT(CellFsStat, st_gid, 8);
AT(CellFsStat, st_atime, 12);
AT(CellFsStat, st_mtime, 20);
AT(CellFsStat, st_ctime, 28);
AT(CellFsStat, st_size, 36);
AT(CellFsStat, st_blksize, 44);

SIZE(CellFsUtimbuf, 16);
ALIGN(CellFsUtimbuf, 4);
AT(CellFsUtimbuf, actime, 0);
AT(CellFsUtimbuf, modtime, 8);

SIZE(CellFsDirectoryEntry, 312);
ALIGN(CellFsDirectoryEntry, 4);
AT(CellFsDirectoryEntry, attribute, 0);
AT(CellFsDirectoryEntry, entry_name, 52);

SIZE(CellFsRingBuffer, 32);
ALIGN(CellFsRingBuffer, 8);
AT(CellFsRingBuffer, ringbuf_size, 0);
AT(CellFsRingBuffer, block_size, 8);
AT(CellFsRingBuffer, transfer_rate, 16);
AT(CellFsRingBuffer, copy, 24);

SIZE(CellFsAio, 40);
ALIGN(CellFsAio, 8);
AT(CellFsAio, fd, 0);
AT(CellFsAio, offset, 8);
AT(CellFsAio, buf, 16);
AT(CellFsAio, size, 24);
AT(CellFsAio, user_data, 32);

/* The buffer is a 32-bit effective address in both data models. */
LAYOUT_ASSERT(sizeof(((CellFsAio *)0)->buf) == 4, "CellFsAio.buf width");

int cellfs_layout_probe(void);
int cellfs_layout_probe(void)
{
    return (int)sizeof(CellFsAio);
}
