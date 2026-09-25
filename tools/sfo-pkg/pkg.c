/* pkg.c - PS3 PKG file utility (C implementation of pkg.py)
 *
 * Supports the same options as pkg.py:
 *   pkg <directory> [outfile]          (pack, requires --contentid)
 *   pkg -l / --list <pkgfile>          (list)
 *   pkg -x / --extract <pkgfile>       (extract)
 *   pkg -c / --contentid <id>
 *   pkg -d / --debug
 *   pkg -v / --version
 *   pkg -h / --help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>
#include <errno.h>

#define PS3PKG_APP_VERSION "0.6"

#ifdef _WIN32
# include <windows.h>
# include <direct.h>
# include <io.h>
# include <wchar.h>
# define PATH_SEP '\\'
# define MKDIR(p) _mkdir(p)
#else
# include <sys/stat.h>
# include <sys/types.h>
# include <dirent.h>
# define PATH_SEP '/'
# define MKDIR(p) mkdir(p, 0755)
#endif

#include "sha1.h"

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */
#define TYPE_NPDRMSELF      0x1
#define TYPE_RAW            0x3
#define TYPE_DIRECTORY      0x4
#define TYPE_OVERWRITE_ALLOWED 0x80000000U

#define PKG_MAGIC           0x7F504B47U

/* Sizes of on-disk structures */
#define PKG_HDR_SIZE        0x80
#define PKG_META_SIZE       0x40
#define PKG_FILE_HDR_SIZE   0x20
#define PKG_DIGEST_SIZE     0x10
#define SELF_HDR_SIZE       0x68   /* 104 bytes */
#define SELF_APPINFO_SIZE   0x18   /* 24 bytes */
#define EBOOT_META_SIZE     0x80

/* ------------------------------------------------------------------ */
/* Debug flag                                                           */
/* ------------------------------------------------------------------ */
static int g_debug = 0;

/* ------------------------------------------------------------------ */
/* Big-endian read/write helpers                                        */
/* ------------------------------------------------------------------ */
static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint64_t rd_be64(const uint8_t *p)
{
    return ((uint64_t)rd_be32(p) << 32) | rd_be32(p + 4);
}

static void wr_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xff);
}

static void wr_be64(uint8_t *p, uint64_t v)
{
    wr_be32(p,     (uint32_t)(v >> 32));
    wr_be32(p + 4, (uint32_t)(v & 0xffffffffU));
}

/* ------------------------------------------------------------------ */
/* SHA1 helper (produces 20-byte digest)                               */
/* ------------------------------------------------------------------ */
/* LOCAL MODIFICATION (PS3 Custom Toolchain): upstream called a bundled
   SHA-1 by Paul E. Jones whose licence we cannot take into an MIT tree.
   This now calls our own implementation (sha1.c, from FIPS 180-4), which
   packs the digest big-endian itself, so upstream's SHA1Finalize helper is
   no longer needed. */
static void sha1_hash(const uint8_t *data, size_t len, uint8_t digest[20])
{
    ps3_sha1(data, len, digest);
}

/* ------------------------------------------------------------------ */
/* PKG stream cipher                                                    */
/* ------------------------------------------------------------------ */

/* Build a 64-byte context from a 16-byte key:
 *   [key[0..7], key[0..7], key[8..15], key[8..15], 0x00 * 32] */
static void key_to_context(const uint8_t *key, uint8_t ctx[64])
{
    memcpy(ctx,      key,     8);
    memcpy(ctx + 8,  key,     8);
    memcpy(ctx + 16, key + 8, 8);
    memcpy(ctx + 24, key + 8, 8);
    memset(ctx + 32, 0,       32);
}

static void set_context_num(uint8_t ctx[64], uint64_t num)
{
    ctx[0x38] = (uint8_t)(num >> 56);
    ctx[0x39] = (uint8_t)(num >> 48);
    ctx[0x3a] = (uint8_t)(num >> 40);
    ctx[0x3b] = (uint8_t)(num >> 32);
    ctx[0x3c] = (uint8_t)(num >> 24);
    ctx[0x3d] = (uint8_t)(num >> 16);
    ctx[0x3e] = (uint8_t)(num >> 8);
    ctx[0x3f] = (uint8_t)(num);
}

static void manipulate_context(uint8_t ctx[64])
{
    uint64_t num =
        ((uint64_t)ctx[0x38] << 56) | ((uint64_t)ctx[0x39] << 48) |
        ((uint64_t)ctx[0x3a] << 40) | ((uint64_t)ctx[0x3b] << 32) |
        ((uint64_t)ctx[0x3c] << 24) | ((uint64_t)ctx[0x3d] << 16) |
        ((uint64_t)ctx[0x3e] << 8)  |  (uint64_t)ctx[0x3f];
    num++;
    set_context_num(ctx, num);
}

/* XOR-encrypt/decrypt len bytes of input using the stream cipher.
 * ctx is updated in-place (counter advances).
 * Returns a newly allocated buffer (caller must free), or NULL on OOM. */
static uint8_t *pkg_crypt(uint8_t ctx[64], const uint8_t *input, size_t len)
{
    uint8_t *out = (uint8_t *)malloc(len ? len : 1);
    if (!out) return NULL;

    size_t offset = 0, remaining = len;
    while (remaining > 0) {
        size_t block = remaining > 0x10 ? 0x10 : remaining;
        uint8_t hash[20];
        sha1_hash(ctx, 64, hash);
        for (size_t i = 0; i < block; i++)
            out[offset + i] = hash[i] ^ input[offset + i];
        offset    += block;
        remaining -= block;
        manipulate_context(ctx);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Dynamic byte buffer                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} dynbuf_t;

static int dynbuf_append(dynbuf_t *b, const void *src, size_t n)
{
    if (n == 0) return 0;
    if (b->size + n > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 65536;
        while (newcap < b->size + n) newcap *= 2;
        uint8_t *tmp = (uint8_t *)realloc(b->data, newcap);
        if (!tmp) return -1;
        b->data = tmp;
        b->cap  = newcap;
    }
    memcpy(b->data + b->size, src, n);
    b->size += n;
    return 0;
}

static int dynbuf_append_zeros(dynbuf_t *b, size_t n)
{
    if (n == 0) return 0;
    if (b->size + n > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 65536;
        while (newcap < b->size + n) newcap *= 2;
        uint8_t *tmp = (uint8_t *)realloc(b->data, newcap);
        if (!tmp) return -1;
        b->data = tmp;
        b->cap  = newcap;
    }
    memset(b->data + b->size, 0, n);
    b->size += n;
    return 0;
}

static void dynbuf_free(dynbuf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->size = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* File header entry (in-memory, not the on-disk layout)               */
/* ------------------------------------------------------------------ */
/* LOCAL FIX 3 (PS3 Custom Toolchain, 2026-09-24, t_6858d853):
   Paths are dynamically allocated per entry rather than fixed buffers.
   This removes the FH_MAX_NAME (256) and src_path[512] truncation bugs
   reported in EMP Static relay. */
typedef struct pkg_file_entry {
    char     *filename;     /* relative path, forward slashes (heap-allocated) */
    uint32_t filename_len;
    uint64_t file_off;     /* filled during pack */
    uint64_t file_size;    /* reported size (may differ for NPDRM SELF) */
    uint32_t flags;
    uint32_t file_name_off; /* relative to start of data area */
    char     *src_path;     /* host path for reading (heap-allocated) */
} pkg_file_entry_t;

/* Dynamic file table (grows as needed, no silent 1024 cap) */
static pkg_file_entry_t *g_files = NULL;
static int g_file_count = 0;
static int g_file_cap = 0;

static void free_file_entries(void)
{
    if (g_files) {
        for (int i = 0; i < g_file_count; i++) {
            free(g_files[i].filename);
            free(g_files[i].src_path);
        }
        free(g_files);
        g_files = NULL;
    }
    g_file_count = 0;
    g_file_cap = 0;
}

static int add_file_entry(pkg_file_entry_t **out_entry)
{
    if (g_file_count >= g_file_cap) {
        int new_cap = g_file_cap == 0 ? 1024 : g_file_cap * 2;
        pkg_file_entry_t *new_files = (pkg_file_entry_t *)realloc(g_files, (size_t)new_cap * sizeof(pkg_file_entry_t));
        if (!new_files) {
            fprintf(stderr, "pkg: out of memory allocating file table (capacity %d)\n", new_cap);
            return -1;
        }
        g_files = new_files;
        g_file_cap = new_cap;
    }
    *out_entry = &g_files[g_file_count++];
    memset(*out_entry, 0, sizeof(pkg_file_entry_t));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Windows extended-length path helpers                               */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static wchar_t *win32_to_extended_wpath(const char *path)
{
    if (!path || !*path) return NULL;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) {
        wlen = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
        if (wlen <= 0) return NULL;
    }
    wchar_t *wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wpath) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen) <= 0) {
        MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, wlen);
    }

    for (wchar_t *p = wpath; *p; p++) {
        if (*p == L'/') *p = L'\\';
    }

    if (wcsncmp(wpath, L"\\\\?\\", 4) == 0 || wcsncmp(wpath, L"\\??\\", 4) == 0) {
        return wpath;
    }

    DWORD full_len = GetFullPathNameW(wpath, 0, NULL, NULL);
    if (full_len == 0) {
        free(wpath);
        return NULL;
    }

    wchar_t *full = (wchar_t *)malloc(((size_t)full_len + 8) * sizeof(wchar_t));
    if (!full) {
        free(wpath);
        return NULL;
    }
    GetFullPathNameW(wpath, full_len, full, NULL);
    free(wpath);

    if (wcsncmp(full, L"\\\\?\\", 4) == 0) {
        return full;
    }

    if (full[0] == L'\\' && full[1] == L'\\') {
        size_t elen = wcslen(full) + 8;
        wchar_t *unc = (wchar_t *)malloc(elen * sizeof(wchar_t));
        if (unc) swprintf(unc, elen, L"\\\\?\\UNC%ls", full + 1);
        free(full);
        return unc;
    } else {
        size_t elen = wcslen(full) + 5;
        wchar_t *drv = (wchar_t *)malloc(elen * sizeof(wchar_t));
        if (drv) swprintf(drv, elen, L"\\\\?\\%ls", full);
        free(full);
        return drv;
    }
}

static char *win32_wide_to_utf8(const wchar_t *wstr)
{
    if (!wstr) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *str = (char *)malloc((size_t)len);
    if (!str) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str, len, NULL, NULL);
    return str;
}
#endif

/* ------------------------------------------------------------------ */
/* File I/O helpers                                                    */
/* ------------------------------------------------------------------ */
static uint8_t *read_file_alloc(const char *path, size_t *out_len)
{
    *out_len = 0;
#ifdef _WIN32
    wchar_t *wpath = win32_to_extended_wpath(path);
    FILE *fp = wpath ? _wfopen(wpath, L"rb") : NULL;
    free(wpath);
#else
    FILE *fp = fopen(path, "rb");
#endif
    if (!fp) { perror(path); return NULL; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long len = ftell(fp);
    if (len < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if (rd != (size_t)len) { free(buf); return NULL; }
    buf[len] = 0;
    *out_len = (size_t)len;
    return buf;
}

static void delete_file(const char *path)
{
    if (!path || !*path) return;
#ifdef _WIN32
    wchar_t *w = win32_to_extended_wpath(path);
    if (w) {
        DeleteFileW(w);
        free(w);
    }
#else
    remove(path);
#endif
}

/* Create all intermediate directories (no fixed 512 buffer) */
static int make_dirs(const char *path)
{
    if (!path || !*path) return 0;
    char *tmp = strdup(path);
    if (!tmp) return -1;
    size_t len = strlen(tmp);

    for (size_t i = 0; i < len; i++) {
        if (tmp[i] == '\\') tmp[i] = '/';
    }

    size_t start = 1;
    if (len >= 3 && tmp[1] == ':' && tmp[2] == '/')
        start = 3;
    else if (strncmp(tmp, "//?/", 4) == 0)
        start = 4;

    for (size_t i = start; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
#ifdef _WIN32
            wchar_t *w = win32_to_extended_wpath(tmp);
            if (w) {
                CreateDirectoryW(w, NULL);
                free(w);
            }
#else
            mkdir(tmp, 0755);
#endif
            tmp[i] = '/';
        }
    }
#ifdef _WIN32
    wchar_t *w = win32_to_extended_wpath(tmp);
    if (w) {
        CreateDirectoryW(w, NULL);
        free(w);
    }
#else
    mkdir(tmp, 0755);
#endif
    free(tmp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Directory traversal (for pack)                                      */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
static int get_files_win32(const wchar_t *wfolder, const wchar_t *woriginal)
{
    /* LOCAL FIX (PS3 Custom Toolchain): the POSIX branch below guards against
       a folder argument that already ends in a separator; this one did not,
       so a trailing slash produced "<folder>/\<name>" and the relative path
       kept a leading slash after prefix stripping. That is not cosmetic: the
       EBOOT.BIN test a few lines down is an exact strcmp against
       "USRDIR/EBOOT.BIN", so "/USRDIR/EBOOT.BIN" missed it and the entry was
       written as TYPE_RAW instead of TYPE_NPDRMSELF, skipping the NPDRM size
       rounding as well. ps3_add_pkg passes "<dir>/" with the trailing slash,
       so every .pkg built on Windows hit this; the Linux build did not.
       Candidate for an upstream PR (none filed yet); see
       tools/sfo-pkg/PROVENANCE.md. */
    size_t wlen = wcslen(wfolder);
    int folder_has_sep = wlen > 0 &&
        (wfolder[wlen - 1] == L'/' || wfolder[wlen - 1] == L'\\');

    wchar_t *pattern = (wchar_t *)malloc((wlen + 3) * sizeof(wchar_t));
    if (!pattern) return -1;
    if (folder_has_sep)
        swprintf(pattern, wlen + 3, L"%ls*", wfolder);
    else
        swprintf(pattern, wlen + 3, L"%ls\\*", wfolder);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return 0;
        char *utf8_folder = win32_wide_to_utf8(wfolder);
        fprintf(stderr, "pkg: cannot open directory %s (error %lu)\n",
                utf8_folder ? utf8_folder : "?", (unsigned long)err);
        free(utf8_folder);
        return -1;
    }

    /* Collect entries: files first, then directories.
       LOCAL FIX (PS3 Custom Toolchain): heap-allocated, NOT stack arrays.
       At [MAX_FILES][260] these two tables are ~532 KB together per
       recursion frame; against the linked 2 MB stack reserve (mingw's
       default — measured from the shipped exe's PE header,
       SizeOfStackReserve 0x200000) the FOURTH frame overflows, i.e. a
       tree three subdirectories deep counting from the packaged root
       (the root walk is frame one).  Crashed 0xC00000FD before
       producing any output; found by the first external consumer
       packaging USRDIR/Data/SkyBox; upstream carries the same frames.
       They stay live across the recursion below, so they are freed
       only at the end of this call.
       LOCAL FIX 3: wide character strings and dynamic reallocation per entry. */
    wchar_t **files_list = NULL;
    int nfiles = 0, cap_files = 0;
    wchar_t **dirs_list = NULL;
    int ndirs = 0, cap_dirs = 0;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        size_t namelen = wcslen(fd.cFileName);
        size_t fulllen = wlen + 1 + namelen + 1;
        wchar_t *full = (wchar_t *)malloc(fulllen * sizeof(wchar_t));
        if (!full) {
            FindClose(h);
            goto err_cleanup;
        }
        if (folder_has_sep)
            swprintf(full, fulllen, L"%ls%ls", wfolder, fd.cFileName);
        else
            swprintf(full, fulllen, L"%ls\\%ls", wfolder, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (ndirs >= cap_dirs) {
                int ncap = cap_dirs == 0 ? 128 : cap_dirs * 2;
                wchar_t **nd = (wchar_t **)realloc(dirs_list, (size_t)ncap * sizeof(wchar_t *));
                if (!nd) { free(full); FindClose(h); goto err_cleanup; }
                dirs_list = nd;
                cap_dirs = ncap;
            }
            dirs_list[ndirs++] = full;
        } else {
            if (nfiles >= cap_files) {
                int ncap = cap_files == 0 ? 128 : cap_files * 2;
                wchar_t **nf = (wchar_t **)realloc(files_list, (size_t)ncap * sizeof(wchar_t *));
                if (!nf) { free(full); FindClose(h); goto err_cleanup; }
                files_list = nf;
                cap_files = ncap;
            }
            files_list[nfiles++] = full;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    /* Process files */
    size_t orig_len = wcslen(woriginal);
    for (int i = 0; i < nfiles; i++) {
        const wchar_t *wrel = files_list[i] + orig_len;
        while (*wrel == L'/' || *wrel == L'\\') wrel++;
        char *rel = win32_wide_to_utf8(wrel);
        if (!rel) goto err_cleanup;
        for (char *p = rel; *p; p++) {
            if (*p == '\\') *p = '/';
        }

        LARGE_INTEGER fsz;
        HANDLE fh = CreateFileW(files_list[i], GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (fh == INVALID_HANDLE_VALUE) {
            char *full_utf8 = win32_wide_to_utf8(files_list[i]);
            fprintf(stderr, "Cannot open: %s\n", full_utf8 ? full_utf8 : rel);
            free(full_utf8);
            free(rel);
            goto err_cleanup;
        }
        GetFileSizeEx(fh, &fsz);
        CloseHandle(fh);

        pkg_file_entry_t *e = NULL;
        if (add_file_entry(&e) != 0) {
            free(rel);
            goto err_cleanup;
        }
        e->filename = rel;
        e->filename_len = (uint32_t)strlen(rel);
        e->file_size = (uint64_t)fsz.QuadPart;
        e->src_path = win32_wide_to_utf8(files_list[i]);

        if (strcmp(rel, "USRDIR/EBOOT.BIN") == 0) {
            e->file_size = ((e->file_size - 0x30 + 63) & ~(uint64_t)63) + 0x30;
            e->flags = TYPE_OVERWRITE_ALLOWED | TYPE_NPDRMSELF;
        } else {
            e->flags = TYPE_OVERWRITE_ALLOWED | TYPE_RAW;
        }
        free(files_list[i]);
        files_list[i] = NULL;
    }
    free(files_list);
    files_list = NULL;

    /* Process directories */
    for (int i = 0; i < ndirs; i++) {
        const wchar_t *wrel = dirs_list[i] + orig_len;
        while (*wrel == L'/' || *wrel == L'\\') wrel++;
        char *rel = win32_wide_to_utf8(wrel);
        if (!rel) goto err_cleanup;
        for (char *p = rel; *p; p++) {
            if (*p == '\\') *p = '/';
        }

        pkg_file_entry_t *e = NULL;
        if (add_file_entry(&e) != 0) {
            free(rel);
            goto err_cleanup;
        }
        e->filename = rel;
        e->filename_len = (uint32_t)strlen(rel);
        e->file_size = 0;
        e->flags = TYPE_OVERWRITE_ALLOWED | TYPE_DIRECTORY;
        e->src_path = NULL;

        int rc = get_files_win32(dirs_list[i], woriginal);
        free(dirs_list[i]);
        dirs_list[i] = NULL;
        if (rc != 0) {
            goto err_cleanup;
        }
    }
    free(dirs_list);
    dirs_list = NULL;
    return 0;

err_cleanup:
    if (files_list) {
        for (int i = 0; i < nfiles; i++) {
            free(files_list[i]);
            files_list[i] = NULL;
        }
        free(files_list);
        files_list = NULL;
    }
    if (dirs_list) {
        for (int i = 0; i < ndirs; i++) {
            free(dirs_list[i]);
            dirs_list[i] = NULL;
        }
        free(dirs_list);
        dirs_list = NULL;
    }
    return -1;
}

static int get_files(const char *folder, const char *original)
{
    (void)original;
    wchar_t *wfolder = win32_to_extended_wpath(folder);
    if (!wfolder) {
        fprintf(stderr, "pkg: cannot resolve folder %s\n", folder);
        return -1;
    }
    int rc = get_files_win32(wfolder, wfolder);
    free(wfolder);
    return rc;
}
#else  /* POSIX */
static int collect_dir(const char *folder, const char *original)
{
    DIR *dp = opendir(folder);
    if (!dp) {
        perror(folder);
        return -1;
    }

    /* Collect names.
       LOCAL FIX (PS3 Custom Toolchain): heap-allocated, NOT stack arrays —
       at [MAX_FILES][512] these two tables are 1 MB together per recursion
       level; see the Win32 branch's note for the depth-3 stack-overflow
       this caused there.  Linux's larger default stack merely hid the same
       bug to greater depth.  Freed at the end of this call, after the
       recursion that keeps them live.
       LOCAL FIX 3: dynamic array growth and heap paths removing 512-byte limit. */
    char **files_list = NULL;
    int nfiles = 0, cap_files = 0;
    char **dirs_list = NULL;
    int ndirs = 0, cap_dirs = 0;

    size_t foldlen = strlen(folder);
    int folder_has_sep = foldlen > 0 &&
        (folder[foldlen-1] == '/' || folder[foldlen-1] == '\\');

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        size_t dlen = strlen(de->d_name);
        size_t fulllen = foldlen + 1 + dlen + 1;
        char *full = (char *)malloc(fulllen);
        if (!full) {
            fprintf(stderr, "pkg: out of memory allocating path\n");
            closedir(dp);
            goto err_cleanup;
        }
        if (folder_has_sep)
            snprintf(full, fulllen, "%s%s", folder, de->d_name);
        else
            snprintf(full, fulllen, "%s/%s", folder, de->d_name);

        struct stat st;
        if (stat(full, &st) != 0) {
            fprintf(stderr, "Cannot open: %s\n", full);
            free(full);
            closedir(dp);
            goto err_cleanup;
        }

        if (S_ISDIR(st.st_mode)) {
            if (ndirs >= cap_dirs) {
                int ncap = cap_dirs == 0 ? 128 : cap_dirs * 2;
                char **nd = (char **)realloc(dirs_list, (size_t)ncap * sizeof(char *));
                if (!nd) { free(full); closedir(dp); goto err_cleanup; }
                dirs_list = nd;
                cap_dirs = ncap;
            }
            dirs_list[ndirs++] = full;
        } else {
            if (nfiles >= cap_files) {
                int ncap = cap_files == 0 ? 128 : cap_files * 2;
                char **nf = (char **)realloc(files_list, (size_t)ncap * sizeof(char *));
                if (!nf) { free(full); closedir(dp); goto err_cleanup; }
                files_list = nf;
                cap_files = ncap;
            }
            files_list[nfiles++] = full;
        }
    }
    closedir(dp);

    /* Sort files and dirs alphabetically (to match glob.glob behavior) */
    for (int i = 0; i < nfiles - 1; i++) {
        for (int j = i + 1; j < nfiles; j++) {
            if (strcmp(files_list[i], files_list[j]) > 0) {
                char *tmp = files_list[i];
                files_list[i] = files_list[j];
                files_list[j] = tmp;
            }
        }
    }
    for (int i = 0; i < ndirs - 1; i++) {
        for (int j = i + 1; j < ndirs; j++) {
            if (strcmp(dirs_list[i], dirs_list[j]) > 0) {
                char *tmp = dirs_list[i];
                dirs_list[i] = dirs_list[j];
                dirs_list[j] = tmp;
            }
        }
    }

    /* Files first */
    size_t orig_len = strlen(original);
    for (int i = 0; i < nfiles; i++) {
        /* Build relative path (strip original prefix, normalise slashes) */
        const char *rel_start = files_list[i] + orig_len;
        while (*rel_start == '/' || *rel_start == '\\') rel_start++;
        char *newpath = strdup(rel_start);
        if (!newpath) goto err_cleanup;
        for (size_t k = 0; newpath[k]; k++) {
            if (newpath[k] == '\\') newpath[k] = '/';
        }

        struct stat st;
        if (stat(files_list[i], &st) != 0) {
            fprintf(stderr, "Cannot open: %s\n", files_list[i]);
            free(newpath);
            goto err_cleanup;
        }

        pkg_file_entry_t *e = NULL;
        if (add_file_entry(&e) != 0) {
            free(newpath);
            goto err_cleanup;
        }
        e->filename = newpath;
        e->filename_len = (uint32_t)strlen(newpath);
        e->file_size = (uint64_t)st.st_size;
        e->src_path = strdup(files_list[i]);
        if (!e->src_path) goto err_cleanup;

        if (strcmp(newpath, "USRDIR/EBOOT.BIN") == 0) {
            e->file_size = ((e->file_size - 0x30 + 63) & ~(uint64_t)63) + 0x30;
            e->flags = TYPE_OVERWRITE_ALLOWED | TYPE_NPDRMSELF;
        } else {
            e->flags = TYPE_OVERWRITE_ALLOWED | TYPE_RAW;
        }
        free(files_list[i]);
        files_list[i] = NULL;
    }
    free(files_list);
    files_list = NULL;

    /* Directories second */
    for (int i = 0; i < ndirs; i++) {
        const char *rel_start = dirs_list[i] + orig_len;
        while (*rel_start == '/' || *rel_start == '\\') rel_start++;
        char *newpath = strdup(rel_start);
        if (!newpath) goto err_cleanup;
        for (size_t k = 0; newpath[k]; k++) {
            if (newpath[k] == '\\') newpath[k] = '/';
        }

        pkg_file_entry_t *e = NULL;
        if (add_file_entry(&e) != 0) {
            free(newpath);
            goto err_cleanup;
        }
        e->filename = newpath;
        e->filename_len = (uint32_t)strlen(newpath);
        e->file_size = 0;
        e->flags = TYPE_OVERWRITE_ALLOWED | TYPE_DIRECTORY;
        e->src_path = NULL;

        int rc = collect_dir(dirs_list[i], original);
        free(dirs_list[i]);
        dirs_list[i] = NULL;
        if (rc != 0) {
            goto err_cleanup;
        }
    }
    free(dirs_list);
    dirs_list = NULL;
    return 0;

err_cleanup:
    if (files_list) {
        for (int i = 0; i < nfiles; i++) {
            free(files_list[i]);
            files_list[i] = NULL;
        }
        free(files_list);
        files_list = NULL;
    }
    if (dirs_list) {
        for (int i = 0; i < ndirs; i++) {
            free(dirs_list[i]);
            dirs_list[i] = NULL;
        }
        free(dirs_list);
        dirs_list = NULL;
    }
    return -1;
}

static int get_files(const char *folder, const char *original)
{
    return collect_dir(folder, original);
}
#endif

/* ------------------------------------------------------------------ */
/* Write a pkg_file_header entry into raw bytes (big-endian)           */
/* ------------------------------------------------------------------ */
static void write_file_hdr(uint8_t out[PKG_FILE_HDR_SIZE],
                            const pkg_file_entry_t *e)
{
    wr_be32(out,       e->file_name_off);
    wr_be32(out + 4,   e->filename_len);
    wr_be64(out + 8,   e->file_off);
    wr_be64(out + 16,  e->file_size);
    wr_be32(out + 24,  e->flags);
    wr_be32(out + 28,  0); /* padding */
}

/* ------------------------------------------------------------------ */
/* list_pkg                                                            */
/* ------------------------------------------------------------------ */
static int list_pkg(const char *filename)
{
    size_t data_len;
    uint8_t *data = read_file_alloc(filename, &data_len);
    if (!data) return 1;

    if (data_len < PKG_HDR_SIZE) {
        fprintf(stderr, "PKG: file too small\n");
        free(data);
        return 1;
    }

    uint32_t magic       = rd_be32(data);
    uint32_t type        = rd_be32(data + 4);
    uint32_t pkg_info_off = rd_be32(data + 8);
    uint32_t unk1        = rd_be32(data + 12);
    uint32_t head_size   = rd_be32(data + 16);
    uint32_t item_count  = rd_be32(data + 20);
    uint64_t package_size = rd_be64(data + 24);
    uint64_t data_off    = rd_be64(data + 32);
    uint64_t data_size   = rd_be64(data + 40);

    const uint8_t *content_id = data + 48;          /* 0x30 bytes */
    const uint8_t *qa_digest  = data + 48 + 0x30;   /* 0x10 bytes */
    const uint8_t *k_licensee = data + 48 + 0x40;   /* 0x10 bytes */

    /* Compute decrypted K Licensee for display */
    uint8_t ctx[64];
    key_to_context(qa_digest, ctx);
    set_context_num(ctx, 0xFFFFFFFFFFFFFFFFULL);
    uint8_t *licensee_dec = pkg_crypt(ctx, k_licensee, 0x10);

    /* QA_Digest: each byte as uppercase hex without zero-pad (Python %X) */
    char qa_str[33] = {0};
    for (int i = 0; i < 0x10 && i * 2 < 32; i++) {
        if (qa_digest[i] == 0) break;
        char tmp[4];
        snprintf(tmp, sizeof(tmp), "%X", qa_digest[i]);
        strcat(qa_str, tmp);
    }

    char cid[0x31] = {0};
    memcpy(cid, content_id, 0x30);
    cid[0x30] = '\0';
    for (int i = 0; i < 0x30; i++) { if (!cid[i]) break; }

    printf("[X] Magic: %08x\n", magic);
    printf("[X] Type: %08x\n", type);
    printf("[X] Offset to package info: %08x\n", pkg_info_off);
    printf("[ ] unk1: %08x\n", unk1);
    printf("[X] Head Size: %08x\n", head_size);
    printf("[X] Item Count: %08x\n", item_count);
    printf("[X] Package Size: %016llx\n", (unsigned long long)package_size);
    printf("[X] Data Offset: %016llx\n", (unsigned long long)data_off);
    printf("[X] Data Size: %016llx\n", (unsigned long long)data_size);
    printf("[X] ContentID: '%s'\n", cid);
    printf("[X] QA_Digest: %s\n", qa_str);
    if (licensee_dec) {
        printf("[X] K Licensee: ");
        for (int i = 0; i < 0x10; i++) printf("%02x", licensee_dec[i]);
        putchar('\n');
        free(licensee_dec);
    }
    putchar('\n');

    if (type != 0x00000001) {
        fprintf(stderr, "Unsupported Type\n");
        free(data);
        return 1;
    }

    if (item_count == 0) { free(data); return 0; }

    printf("Listing: \"%s\"\n", filename);
    puts("+) overwrite, -) no overwrite");
    putchar('\n');

    const uint8_t *data_enc = data + data_off;
    if (data_off + (uint64_t)PKG_FILE_HDR_SIZE * item_count > data_len) {
        fprintf(stderr, "PKG: data area truncated\n");
        free(data);
        return 1;
    }

    /* Decrypt file descriptors */
    uint8_t ctx2[64];
    key_to_context(qa_digest, ctx2);
    size_t desc_bytes = (size_t)PKG_FILE_HDR_SIZE * item_count;
    uint8_t *dec_descs = pkg_crypt(ctx2, data_enc, desc_bytes);
    if (!dec_descs) { free(data); return 1; }

    for (uint32_t i = 0; i < item_count; i++) {
        const uint8_t *fh = dec_descs + i * PKG_FILE_HDR_SIZE;
        uint32_t fn_off  = rd_be32(fh);
        uint32_t fn_len  = rd_be32(fh + 4);
        uint64_t fsize   = rd_be64(fh + 16);
        uint32_t flags   = rd_be32(fh + 24);

        if ((uint64_t)data_off + fn_off + fn_len > data_len) {
            fprintf(stderr, "PKG: filename data out of bounds\n");
            free(dec_descs);
            free(data);
            return 1;
        }

        uint8_t *name_dec = pkg_crypt(ctx2, data_enc + fn_off, fn_len);
        if (name_dec) {
            char *name = (char *)malloc((size_t)fn_len + 1);
            if (name) {
                memcpy(name, name_dec, fn_len);
                name[fn_len] = '\0';

                char line[64];
                if ((flags & 0xFF) == TYPE_NPDRMSELF)
                    strcpy(line, " NPDRM SELF:");
                else if ((flags & 0xFF) == TYPE_DIRECTORY)
                    strcpy(line, "  directory:");
                else if ((flags & 0xFF) == TYPE_RAW)
                    strcpy(line, "   raw data:");
                else
                    strcpy(line, "    unknown:");

                printf("%s%c%11llu: %s\n",
                       line,
                       (flags & TYPE_OVERWRITE_ALLOWED) ? '+' : '-',
                       (unsigned long long)fsize,
                       name);
                free(name);
            }
            free(name_dec);
        }
    }

    free(dec_descs);
    free(data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* unpack (extract)                                                    */
/* ------------------------------------------------------------------ */
static int unpack_pkg(const char *filename)
{
    size_t data_len;
    uint8_t *data = read_file_alloc(filename, &data_len);
    if (!data) return 1;

    if (data_len < PKG_HDR_SIZE) {
        fprintf(stderr, "PKG: file too small\n");
        free(data);
        return 1;
    }

    uint32_t type        = rd_be32(data + 4);
    uint32_t item_count  = rd_be32(data + 20);
    uint64_t data_off    = rd_be64(data + 32);
    uint64_t data_size   = rd_be64(data + 40);
    const uint8_t *content_id = data + 48;          /* 0x30 bytes */
    const uint8_t *qa_digest  = data + 48 + 0x30;   /* 0x10 bytes */

    if (type != 0x00000001) {
        fprintf(stderr, "Unsupported Type\n");
        free(data);
        return 1;
    }
    if (item_count == 0) { free(data); return 0; }

    if (g_debug) {
        uint32_t magic       = rd_be32(data);
        uint32_t type        = rd_be32(data + 4);
        uint32_t pkg_info_off = rd_be32(data + 8);
        uint32_t unk1        = rd_be32(data + 12);
        uint32_t head_size   = rd_be32(data + 16);
        uint64_t package_size = rd_be64(data + 24);

        printf("[X] Magic: %08x\n", magic);
        printf("[X] Type: %08x\n", type);
        printf("[X] Offset to package info: %08x\n", pkg_info_off);
        printf("[ ] unk1: %08x\n", unk1);
        printf("[X] Head Size: %08x\n", head_size);
        printf("[X] Item Count: %08x\n", item_count);
        printf("[X] Package Size: %016llx\n", (unsigned long long)package_size);
        printf("[X] Data Offset: %016llx\n", (unsigned long long)data_off);
        printf("[X] Data Size: %016llx\n", (unsigned long long)data_size);
        printf("[X] ContentID: '%.48s'\n\n", content_id);
    }

    char dir[0x31] = {0};
    memcpy(dir, content_id, 0x30);
    if (make_dirs(dir) != 0) {
        free(data);
        return 1;
    }

    const uint8_t *data_enc = data + data_off;
    size_t enc_len = (size_t)data_size;
    if (data_off + data_size > data_len) enc_len = data_len - (size_t)data_off;

    uint8_t ctx[64];
    key_to_context(qa_digest, ctx);
    uint8_t *dec_data = pkg_crypt(ctx, data_enc, enc_len);
    if (!dec_data) { free(data); return 1; }

    for (uint32_t i = 0; i < item_count; i++) {
        const uint8_t *fh = dec_data + i * PKG_FILE_HDR_SIZE;
        uint32_t fn_off  = rd_be32(fh);
        uint32_t fn_len  = rd_be32(fh + 4);
        uint64_t foff    = rd_be64(fh + 8);
        uint64_t fsize   = rd_be64(fh + 16);
        uint32_t flags   = rd_be32(fh + 24);

        if (fn_off + fn_len > enc_len) {
            fprintf(stderr, "PKG: filename offset out of range\n");
            free(dec_data);
            free(data);
            return 1;
        }

        char *name = (char *)malloc((size_t)fn_len + 1);
        if (!name) {
            free(dec_data);
            free(data);
            return 1;
        }
        memcpy(name, dec_data + fn_off, fn_len);
        name[fn_len] = '\0';

        size_t outpath_len = strlen(dir) + 1 + fn_len + 1;
        char *outpath = (char *)malloc(outpath_len);
        if (!outpath) {
            free(name);
            free(dec_data);
            free(data);
            return 1;
        }
        snprintf(outpath, outpath_len, "%s/%s", dir, name);

        if ((flags & 0xFF) == TYPE_DIRECTORY) {
            make_dirs(outpath);
        } else {
            char *parent = strdup(outpath);
            char *last_sep = strrchr(parent, '/');
            if (last_sep) { *last_sep = '\0'; make_dirs(parent); }
            free(parent);

#ifdef _WIN32
            wchar_t *wout = win32_to_extended_wpath(outpath);
            FILE *fp = wout ? _wfopen(wout, L"wb") : NULL;
            free(wout);
#else
            FILE *fp = fopen(outpath, "wb");
#endif
            if (!fp) {
                perror(outpath);
                free(name);
                free(outpath);
                free(dec_data);
                free(data);
                return 1;
            }
            if (foff + fsize <= enc_len) {
                size_t wr = fwrite(dec_data + foff, 1, (size_t)fsize, fp);
                if (wr != (size_t)fsize) {
                    perror("fwrite");
                    fclose(fp);
                    delete_file(outpath);
                    free(name);
                    free(outpath);
                    free(dec_data);
                    free(data);
                    return 1;
                }
            }
            int flush_rc = fflush(fp);
            int close_rc = fclose(fp);
            if (flush_rc != 0 || close_rc != 0) {
                perror(outpath);
                delete_file(outpath);
                free(name);
                free(outpath);
                free(dec_data);
                free(data);
                return 1;
            }
        }

        if (g_debug) {
            printf("[X] File Name: %s [%s]%s Overwrite %s\n",
                   name,
                   (flags & 0xFF) == TYPE_NPDRMSELF ? "NPDRM Self" :
                   (flags & 0xFF) == TYPE_DIRECTORY  ? "Directory"  :
                   (flags & 0xFF) == TYPE_RAW         ? "Raw Data"   : "Unknown",
                   (flags & TYPE_OVERWRITE_ALLOWED) ? " " : " NOT",
                   (flags & TYPE_OVERWRITE_ALLOWED) ? "allowed." : "allowed.");

            printf("[X] File Name offset: %08x\n", fn_off);
            printf("[X] File Name Length: %08x\n", fn_len);
            printf("[X] Offset To File Data: %016llx\n", (unsigned long long)foff);
            printf("[X] File Size: %016llx\n", (unsigned long long)fsize);
            printf("[X] Flags: %08x\n\n", flags);
        }

        free(name);
        free(outpath);
    }

    free(dec_data);
    free(data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* pack                                                                */
/* ------------------------------------------------------------------ */

/* SELF magic: first 9 bytes of a fake-SELF file */
static const uint8_t SELF_MAGIC[9] = {
    0x53, 0x43, 0x45, 0x00,   /* "SCE\0" */
    0x00, 0x00, 0x00, 0x02,   /* headerVer = 2 BE */
    0x80                       /* first byte of flags = 0x8000 BE */
};

static int pack_pkg(const char *folder, const char *contentid,
                    const char *outname)
{
    /* -------------------------------------------------------------- */
    /* Gather files                                                     */
    /* -------------------------------------------------------------- */
    free_file_entries();
    int rc = get_files(folder, folder);
    if (rc != 0) {
        free_file_entries();
        return 1;
    }

    int item_count = g_file_count;

    /* -------------------------------------------------------------- */
    /* Build the data-to-encrypt buffer                                */
    /* -------------------------------------------------------------- */
    dynbuf_t buf = {0};

    /* Step 1: assign fileNameOff values.
     * File name area starts at 0x20 * item_count. */
    uint64_t fn_area_off = (uint64_t)PKG_FILE_HDR_SIZE * item_count;
    uint64_t fn_cur      = fn_area_off;

    for (int i = 0; i < item_count; i++) {
        g_files[i].file_name_off = (uint32_t)fn_cur;
        uint32_t aligned = (g_files[i].filename_len + 0x0f) & ~0x0fu;
        fn_cur += aligned;
    }

    /* Step 2: assign fileOff values (after all names). */
    uint64_t data_cur = fn_cur;
    for (int i = 0; i < item_count; i++) {
        g_files[i].file_off = data_cur;
        if ((g_files[i].flags & 0xff) != TYPE_DIRECTORY) {
            uint64_t padded = (g_files[i].file_size + 0x0fULL) & ~0x0fULL;
            data_cur += padded;
        }
    }

    /* Step 3: write file descriptors */
    for (int i = 0; i < item_count; i++) {
        uint8_t fh[PKG_FILE_HDR_SIZE];
        write_file_hdr(fh, &g_files[i]);
        if (dynbuf_append(&buf, fh, PKG_FILE_HDR_SIZE) != 0) {
            fprintf(stderr, "pkg: out of memory building header\n");
            dynbuf_free(&buf);
            free_file_entries();
            return 1;
        }
    }

    /* Step 4: write file names (0x10-aligned each) */
    for (int i = 0; i < item_count; i++) {
        uint32_t aligned = (g_files[i].filename_len + 0x0f) & ~0x0fu;
        if (dynbuf_append(&buf, (const uint8_t *)g_files[i].filename, g_files[i].filename_len) != 0 ||
            dynbuf_append_zeros(&buf, aligned - g_files[i].filename_len) != 0) {
            fprintf(stderr, "pkg: out of memory building file names\n");
            dynbuf_free(&buf);
            free_file_entries();
            return 1;
        }
    }

    size_t file_desc_length = buf.size; /* size of header area */

    /* Step 5: write file data */
    for (int i = 0; i < item_count; i++) {
        if ((g_files[i].flags & 0xff) == TYPE_DIRECTORY)
            continue;

        size_t file_data_len = 0;
        uint8_t *file_data = read_file_alloc(g_files[i].src_path, &file_data_len);
        if (!file_data) {
            fprintf(stderr, "Cannot open: %s\n", g_files[i].src_path);
            dynbuf_free(&buf);
            free_file_entries();
            return 1;
        }

        /* SHA1 of this file for EbootMeta */
        uint8_t file_sha1[20];
        sha1_hash(file_data, file_data_len, file_sha1);

        /* Check if SELF / NPDRM */
        int is_npdrm_self = 0;
        size_t digest_off_in_file = 0;

        if (file_data_len >= 9 && memcmp(file_data, SELF_MAGIC, 9) == 0 &&
            file_data_len >= SELF_HDR_SIZE) {
            uint64_t app_info_off  = rd_be64(file_data + 40);
            uint64_t digest_off_hdr = rd_be64(file_data + 88);

            uint32_t app_type = 0;
            if (app_info_off + SELF_APPINFO_SIZE <= file_data_len)
                app_type = rd_be32(file_data + (size_t)app_info_off + 12);

            int found = 0;
            size_t doff = (size_t)digest_off_hdr;
            while (doff + PKG_DIGEST_SIZE <= file_data_len) {
                uint32_t dtype  = rd_be32(file_data + doff);
                uint32_t dsize  = rd_be32(file_data + doff + 4);
                uint64_t is_next = rd_be64(file_data + doff + 8);
                if (dtype == 3) { found = 1; break; }
                doff += dsize;
                if (is_next != 1) break;
            }
            doff += PKG_DIGEST_SIZE;

            if (app_type == 8 && found) {
                is_npdrm_self = 1;
                digest_off_in_file = doff;
            }
        }

        if (is_npdrm_self) {
            dynbuf_append(&buf, file_data, digest_off_in_file);

            uint8_t meta[EBOOT_META_SIZE];
            memset(meta, 0, EBOOT_META_SIZE);
            wr_be32(meta,     0x4E504400U); /* magic "NPD\0" */
            wr_be32(meta + 4, 1);           /* unk1 */
            wr_be32(meta + 8, 3);           /* drmType = 3 (Free) */
            wr_be32(meta + 12, 1);          /* unk2 */
            size_t cid_len = strlen(contentid);
            if (cid_len > 0x30) cid_len = 0x30;
            memcpy(meta + 16, contentid, cid_len);
            for (int j = 0; j < 0x10; j++) {
                meta[0x40 + j] = file_sha1[j];
                meta[0x50 + j] = (~file_sha1[j]) & 0xff;
                uint8_t ns = meta[0x50 + j];
                if (j == 0x0f)
                    meta[0x60 + j] = (1 ^ ns ^ 0xaa) & 0xff;
                else
                    meta[0x60 + j] = (0 ^ ns ^ 0xaa) & 0xff;
            }
            dynbuf_append(&buf, meta, EBOOT_META_SIZE);

            /* Rest of file (skip original 0x80 bytes after digest) */
            if (digest_off_in_file + 0x80 <= file_data_len)
                dynbuf_append(&buf, file_data + digest_off_in_file + 0x80,
                              file_data_len - digest_off_in_file - 0x80);
        } else {
            dynbuf_append(&buf, file_data, file_data_len);
        }

        /* Pad to 0x10-aligned file_size */
        uint64_t fsize_aligned = (g_files[i].file_size + 0x0fULL) & ~0x0fULL;
        if (file_data_len < (size_t)fsize_aligned)
            dynbuf_append_zeros(&buf, (size_t)fsize_aligned - file_data_len);

        free(file_data);
    }

    /* -------------------------------------------------------------- */
    /* Build PKG header                                                 */
    /* -------------------------------------------------------------- */
    uint8_t hdr[PKG_HDR_SIZE];
    memset(hdr, 0, PKG_HDR_SIZE);
    wr_be32(hdr,      PKG_MAGIC);
    wr_be32(hdr + 4,  0x01);
    wr_be32(hdr + 8,  0xC0);   /* pkgInfoOff */
    wr_be32(hdr + 12, 0x05);   /* unk1 */
    wr_be32(hdr + 16, 0x80);   /* headSize */
    wr_be32(hdr + 20, (uint32_t)item_count);
    wr_be64(hdr + 24, buf.size + 0x1A0); /* packageSize */
    wr_be64(hdr + 32, 0x140);  /* dataOff */
    wr_be64(hdr + 40, buf.size); /* dataSize */
    /* contentID */
    size_t cid_len = strlen(contentid);
    if (cid_len > 0x30) cid_len = 0x30;
    memcpy(hdr + 48, contentid, cid_len);
    /* QADigest and KLicensee are zero for now */

    /* QA digest = SHA1(all original file data || header || file descriptor area) */
    /* LOCAL MODIFICATION (PS3 Custom Toolchain): our SHA-1 API, see above. */
    ps3_sha1_ctx qa_ctx;
    ps3_sha1_init(&qa_ctx);

    for (int i = 0; i < item_count; i++) {
        if ((g_files[i].flags & 0xff) == TYPE_DIRECTORY) continue;

        size_t fsize = 0;
        uint8_t *fd = read_file_alloc(g_files[i].src_path, &fsize);
        if (!fd) {
            fprintf(stderr, "Cannot open: %s\n", g_files[i].src_path);
            dynbuf_free(&buf);
            free_file_entries();
            return 1;
        }
        ps3_sha1_update(&qa_ctx, fd, fsize);
        free(fd);
    }

    /* to match pkg.py behavior, uses zeroed contentID */
    ps3_sha1_update(&qa_ctx, hdr, 0x30);
    ps3_sha1_update(&qa_ctx, hdr + 0x60, 0x20);
    ps3_sha1_update(&qa_ctx, hdr + 0x60, 0x20);
    ps3_sha1_update(&qa_ctx, hdr + 0x60, 0x10);

    ps3_sha1_update(&qa_ctx, buf.data, file_desc_length);
    uint8_t qa_digest[20];
    ps3_sha1_final(&qa_ctx, qa_digest);

    /* Store first 16 bytes of QA digest in header */
    memcpy(hdr + 48 + 0x30, qa_digest, 0x10);

    /* KLicensee: encrypt 16 zero bytes with QADigest key, counter=0xFFFFFFFFFFFFFFFF */
    uint8_t klic_ctx[64];
    key_to_context(hdr + 48 + 0x30, klic_ctx);
    set_context_num(klic_ctx, 0xFFFFFFFFFFFFFFFFULL);
    uint8_t zero16[16] = {0};
    uint8_t *licensee = pkg_crypt(klic_ctx, zero16, 0x10);
    if (licensee) {
        memcpy(hdr + 48 + 0x40, licensee, 0x10);
        free(licensee);
    }

    /* -------------------------------------------------------------- */
    /* Build MetaHeader (0x40 bytes, big-endian)                       */
    /* -------------------------------------------------------------- */
    uint8_t meta_hdr[PKG_META_SIZE];
    memset(meta_hdr, 0, PKG_META_SIZE);
    wr_be32(meta_hdr,       1);     /* unk1 */
    wr_be32(meta_hdr + 4,   4);     /* unk2 */
    wr_be32(meta_hdr + 8,   3);     /* drmType = 3 (Free) */
    wr_be32(meta_hdr + 12,  2);     /* unk4 */
    wr_be32(meta_hdr + 16,  4);     /* unk21 */
    wr_be32(meta_hdr + 20,  5);     /* unk22 (5 = gameexec) */
    wr_be32(meta_hdr + 24,  3);     /* unk23 */
    wr_be32(meta_hdr + 28,  4);     /* unk24 */
    wr_be32(meta_hdr + 32,  0x0e);  /* unk31 (0xE = normal) */
    wr_be32(meta_hdr + 36,  4);     /* unk32 */
    wr_be32(meta_hdr + 40,  8);     /* unk33 */
    wr_be16(meta_hdr + 44,  0);     /* secondaryVersion */
    wr_be16(meta_hdr + 46,  0);     /* unk34 */
    wr_be32(meta_hdr + 48,  (uint32_t)buf.size); /* dataSize */
    wr_be32(meta_hdr + 52,  5);     /* unk42 */
    wr_be32(meta_hdr + 56,  4);     /* unk43 */
    wr_be16(meta_hdr + 60,  0x1061); /* packagedBy */
    wr_be16(meta_hdr + 62,  0);     /* packageVersion */

    /* -------------------------------------------------------------- */
    /* Open output file                                                 */
    /* -------------------------------------------------------------- */
    char *final_outname = NULL;
    if (outname) {
        final_outname = strdup(outname);
    } else {
        size_t default_len = strlen(contentid) + 5;
        final_outname = (char *)malloc(default_len);
        if (final_outname) snprintf(final_outname, default_len, "%s.pkg", contentid);
    }
    if (!final_outname) {
        dynbuf_free(&buf);
        free_file_entries();
        return 1;
    }

#ifdef _WIN32
    wchar_t *wout = win32_to_extended_wpath(final_outname);
    FILE *out = wout ? _wfopen(wout, L"wb") : NULL;
    free(wout);
#else
    FILE *out = fopen(final_outname, "wb");
#endif
    if (!out) {
        perror(final_outname);
        free(final_outname);
        dynbuf_free(&buf);
        free_file_entries();
        return 1;
    }

#define WRITE_OUT(ptr, sz, cnt) do { \
    if (fwrite((ptr), (sz), (cnt), out) != (cnt)) { \
        perror("fwrite"); \
        fclose(out); \
        delete_file(final_outname); \
        free(final_outname); \
        dynbuf_free(&buf); \
        free_file_entries(); \
        return 1; \
    } \
} while (0)

    /* Write header (0x80 bytes) */
    WRITE_OUT(hdr, 1, PKG_HDR_SIZE);

    /* Header SHA1[3:19] = 16 bytes */
    uint8_t hdr_sha[20];
    sha1_hash(hdr, PKG_HDR_SIZE, hdr_sha);
    WRITE_OUT(hdr_sha + 3, 1, 16);

    /* MetaBlock SHA1[3:19] + padding + various encrypted pads */
    uint8_t meta_sha[20];
    sha1_hash(meta_hdr, PKG_META_SIZE, meta_sha);
    const uint8_t *metasha16 = meta_sha + 3;

    /* metaBlockSHAPad = 0x30 zero bytes */
    uint8_t meta_sha_pad[0x30];
    memset(meta_sha_pad, 0, 0x30);

    /* Encrypt metaBlockSHAPad with metaSHA key */
    uint8_t ms_ctx[64];
    key_to_context(metasha16, ms_ctx);
    uint8_t *enc1 = pkg_crypt(ms_ctx, meta_sha_pad, 0x30);

    /* Encrypt enc1 with headerSHA key */
    uint8_t hs_ctx[64];
    key_to_context(hdr_sha + 3, hs_ctx);
    uint8_t *enc2 = enc1 ? pkg_crypt(hs_ctx, enc1, 0x30) : NULL;

    if (enc2) WRITE_OUT(enc2, 1, 0x30);
    WRITE_OUT(meta_hdr, 1, PKG_META_SIZE);
    WRITE_OUT(metasha16, 1, 16);
    if (enc1) WRITE_OUT(enc1, 1, 0x30);

    free(enc1);
    free(enc2);

    /* Encrypt and write data */
    uint8_t enc_ctx[64];
    key_to_context(hdr + 48 + 0x30, enc_ctx);
    uint8_t *enc_data = pkg_crypt(enc_ctx, buf.data, buf.size);
    if (enc_data) {
        WRITE_OUT(enc_data, 1, buf.size);
        free(enc_data);
    }

    /* 0x60 trailing zero bytes */
    uint8_t trail[0x60];
    memset(trail, 0, 0x60);
    WRITE_OUT(trail, 1, 0x60);

    int flush_rc = fflush(out);
    int close_rc = fclose(out);
    if (flush_rc != 0 || close_rc != 0) {
        perror(final_outname);
        delete_file(final_outname);
        free(final_outname);
        dynbuf_free(&buf);
        free_file_entries();
        return 1;
    }
#undef WRITE_OUT

    uint64_t data_size_saved = (uint64_t)buf.size;
    dynbuf_free(&buf);

    /* Print header (same as Python's print(header)) */
    {
        uint8_t lic_ctx2[64];
        key_to_context(hdr + 48 + 0x30, lic_ctx2);
        set_context_num(lic_ctx2, 0xFFFFFFFFFFFFFFFFULL);
        uint8_t *lic2 = pkg_crypt(lic_ctx2, hdr + 48 + 0x40, 0x10);

        char cid_str[0x31] = {0};
        memcpy(cid_str, hdr + 48, 0x30);
        char qa_str[33] = {0};
        for (int i = 0; i < 0x10; i++) {
            if (hdr[48 + 0x30 + i] == 0) break;
            char tmp[4];
            snprintf(tmp, sizeof(tmp), "%X", hdr[48 + 0x30 + i]);
            strcat(qa_str, tmp);
        }

        printf("[X] Magic: %08x\n", PKG_MAGIC);
        printf("[X] Type: %08x\n", 0x01);
        printf("[X] Offset to package info: %08x\n", 0xC0);
        printf("[ ] unk1: %08x\n", 0x05);
        printf("[X] Head Size: %08x\n", 0x80);
        printf("[X] Item Count: %08x\n", (uint32_t)item_count);
        printf("[X] Package Size: %016llx\n",
               (unsigned long long)(data_size_saved + 0x1A0));
        printf("[X] Data Offset: %016llx\n", (unsigned long long)0x140ULL);
        printf("[X] Data Size: %016llx\n", (unsigned long long)data_size_saved);
        printf("[X] ContentID: '%s'\n", cid_str);
        printf("[X] QA_Digest: %s\n", qa_str);
        if (lic2) {
            printf("[X] K Licensee: ");
            for (int i = 0; i < 0x10; i++) printf("%02x", lic2[i]);
            putchar('\n');
            free(lic2);
        }
    }

    free(final_outname);
    free_file_entries();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Usage / version                                                     */
/* ------------------------------------------------------------------ */
static void usage(void)
{
    puts("pkg v" PS3PKG_APP_VERSION " - PS3 PKG file utility\n");
    puts("usage:\n"
         "\n"
         "    pkg --contentid=<content-id> target-directory [out-file]\n"
         "\n"
         "    pkg [options] npdrm-package\n"
         "        -l | --list             list packaged files.\n"
         "        -x | --extract          extract package.\n"
         "        -d | --debug            print debug info.\n"
         "\n"
         "    pkg [options]\n"
         "        -c | --contentid        content ID for packing.\n"
         "        -v | --version          print revision.\n"
         "        -h | --help             print this message.");
}

static void version(void)
{
    puts("pkg " PS3PKG_APP_VERSION);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int do_extract   = 0;
    int do_list      = 0;
    char *extract_file = NULL;
    char *list_file    = NULL;
    char *contentid    = NULL;

    static struct option long_opts[] = {
        {"help",      no_argument,       NULL, 'h'},
        {"extract",   required_argument, NULL, 'x'},
        {"debug",     no_argument,       NULL, 'd'},
        {"version",   no_argument,       NULL, 'v'},
        {"list",      required_argument, NULL, 'l'},
        {"contentid", required_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hx:dvl:c:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h': usage();   return 0;
        case 'v': version(); return 0;
        case 'x': extract_file = optarg; do_extract = 1; break;
        case 'l': list_file    = optarg; do_list    = 1; break;
        case 'd': g_debug = 1; break;
        case 'c': contentid = optarg; break;
        default:  usage(); return 2;
        }
    }

    if (do_extract) {
        return unpack_pkg(extract_file);
    } else if (do_list) {
        return list_pkg(list_file);
    } else {
        /* Pack mode: need contentid and 1 or 2 positional args */
        int remaining = argc - optind;
        if (remaining == 1 && contentid) {
            return pack_pkg(argv[optind], contentid, NULL);
        } else if (remaining == 2 && contentid) {
            return pack_pkg(argv[optind], contentid, argv[optind + 1]);
        } else {
            usage();
            return 2;
        }
    }

    return 0;
}
