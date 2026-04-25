/*
 * cell_fs.h — file-system surface for the sys_fs SPRX module.
 *
 * Mirrors the reference SDK split (cell_fs.h re-includes the file API
 * header so single-include callers picking <cell/cell_fs.h> get the
 * full surface).  The errno macros + types live in
 * <cell/fs/cell_fs_file_api.h>; <cell/fs/cell_fs_errno.h> exists for
 * source-compat with samples that include only the errno header.
 *
 * Stub linkage: link `-lfs_stub` (built from
 * tools/nidgen/nids/cellFs.yaml).
 */
#ifndef PS3TC_CELL_CELL_FS_H
#define PS3TC_CELL_CELL_FS_H

#include <cell/fs/cell_fs_file_api.h>

#endif /* PS3TC_CELL_CELL_FS_H */
