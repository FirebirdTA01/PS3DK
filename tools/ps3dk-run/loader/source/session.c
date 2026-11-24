/*
 * ps3dk-loader (protocol 2): one authenticated session - challenge-response, then ordered authenticated
 * commands: a run upload (header, files, verify, stage, spawn) or file-service requests (files.c).
 * Staging is private per run: files land in runs/<id>.partial/, every file is size- and SHA-256-checked, and the
 * directory is renamed to runs/<id>/ only after all of them verify.  Anything else deletes the .partial directory.
 */
#include "loader.h"
#include "../../common/prn_hmac.h"
#include "../../common/prn_net.h"
#include "../../common/prn_sha256.h"
#include "../../common/ps3dk_run_proto.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char path[PRN_MAX_PATH + 1];
    uint64_t size;
    uint8_t sha[32];
} file_entry;

static file_entry files[PRN_MAX_FILES];
static char *args[PRN_MAX_ARGC + 2];
static char argbuf[PRN_MAX_ARGV_BYTES + PRN_MAX_ARGC];
static uint8_t frame[PRN_MAX_FRAME];

static prn_chan chan;

/* before the session key exists: plain REJECTED; afterwards authenticated */
static void reject_plain(int fd, const char *why)
{
    prn_send_frame(fd, PRN_REJECTED, why, (uint32_t)strlen(why));
    loader_status("REJECTED: %s", why);
}

static void reject(int fd, const char *why)
{
    (void)fd;
    prn_chan_send(&chan, PRN_REJECTED, why, (uint32_t)strlen(why));
    loader_status("REJECTED: %s", why);
}

/* rm -rf for a staging directory (files and subdirectories). */
static void remove_tree(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char p[1300];
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
        else unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

void loader_clean_runs(void)
{
    remove_tree(PRN_RUNS_DIR);
    mkdir(PRN_RUNS_DIR, 0777);
}

/* Relative path inside the run directory: no absolute path, no "." or ".." component, no backslash, no empty
 * component, printable ASCII only, and not a duplicate of an earlier entry. */
static int path_ok(const char *p, uint32_t idx)
{
    const char *c = p;
    if (!*p || *p == '/') return 0;
    for (; *c; c++) if (*c < 0x21 || *c > 0x7e || *c == '\\' || *c == ':') return 0;
    for (const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += n + (e ? 1 : 0);
        if (e && !*s) return 0;                    /* trailing slash */
    }
    for (uint32_t i = 0; i < idx; i++) if (!strcmp(files[i].path, p)) return 0;
    return 1;
}

static int mkdirs_for(const char *full)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", full);
    for (char *s = tmp + strlen(PRN_RUNS_DIR) + 1; (s = strchr(s, '/')) != NULL; s++) {
        *s = 0;
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
            struct stat st;
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
        }
        *s = '/';
    }
    return 0;
}

/* A run upload on the authenticated channel: the HEADER payload is in frame[0..len).  Returns 0 to continue the
 * session (rejected or failed upload), -1 to end it.  Does not return when the child was spawned. */
static int run_upload(int fd, uint32_t len, const char *peer_ip, const uint8_t ks[32])
{
    uint8_t type;
    uint32_t nfiles, argc, timeout, m;
    uint16_t cbport;
    uint32_t cbip;
    char cb_ip[16];
    uint64_t total = 0;
    uint32_t run_id = 0;
    char partial[128], final_dir[128], selfpath[384], marker[96];

    if (len < 18) { reject(fd, "HEADER_CB too short"); return 0; }
    const uint8_t *p = frame, *end = frame + len;
    nfiles = prn_get32(p); argc = prn_get32(p + 4); cbport = prn_get16(p + 8); timeout = prn_get32(p + 10); cbip = prn_get32(p + 14); p += 18;
    if (nfiles < 1 || nfiles > PRN_MAX_FILES) { reject(fd, "file count"); return 0; }
    if (argc > PRN_MAX_ARGC) { reject(fd, "argc"); return 0; }
    if (cbport == 0) { reject(fd, "callback port"); return 0; }
    /* the callback address the host chose (the peer address is a router's when one NATs between the console's subnet
       and the host's); 0 = the peer address, all ones rejected */
    if (cbip == 0xffffffffu) { reject(fd, "callback address"); return 0; }
    if (cbip == 0)
        snprintf(cb_ip, sizeof cb_ip, "%s", peer_ip);
    else
        snprintf(cb_ip, sizeof cb_ip, "%u.%u.%u.%u", cbip >> 24, (cbip >> 16) & 255, (cbip >> 8) & 255, cbip & 255);

    /* argv: args[0] is the SELF path, then the user's arguments, then the marker */
    size_t used = 0;
    for (uint32_t i = 0; i < argc; i++) {
        uint16_t n;
        if (end - p < 2) { reject(fd, "argv truncated"); return 0; }
        n = prn_get16(p); p += 2;
        if (end - p < n || used + n + 1 > sizeof argbuf || used + n > PRN_MAX_ARGV_BYTES) { reject(fd, "argv size"); return 0; }
        if (memchr(p, 0, n)) { reject(fd, "NUL in argv"); return 0; }
        memcpy(argbuf + used, p, n); argbuf[used + n] = 0;
        args[i + 1] = argbuf + used;
        used += n + 1; p += n;
    }
    for (uint32_t i = 0; i < nfiles; i++) {
        uint16_t n;
        if (end - p < 2) { reject(fd, "file table truncated"); return 0; }
        n = prn_get16(p); p += 2;
        if (n == 0 || n > PRN_MAX_PATH || end - p < n + 8 + 32) { reject(fd, "file entry"); return 0; }
        memcpy(files[i].path, p, n); files[i].path[n] = 0; p += n;
        files[i].size = prn_get64(p); p += 8;
        memcpy(files[i].sha, p, 32); p += 32;
        if (!path_ok(files[i].path, i)) { reject(fd, "path not allowed"); return 0; }
        if (files[i].size > PRN_MAX_FILE_SIZE) { reject(fd, "file too large"); return 0; }
        total += files[i].size;
        if (total > PRN_MAX_TOTAL_SIZE) { reject(fd, "upload too large"); return 0; }
    }
    if (p != end) { reject(fd, "trailing header bytes"); return 0; }

    run_id = loader_next_run_id();
    snprintf(partial, sizeof partial, "%s/%u.partial", PRN_RUNS_DIR, (unsigned)run_id);
    snprintf(final_dir, sizeof final_dir, "%s/%u", PRN_RUNS_DIR, (unsigned)run_id);
    remove_tree(partial);
    if (mkdir(partial, 0777) != 0) { reject(fd, "cannot create staging dir"); return 0; }
    loader_status("run %u: receiving %u file(s), %llu bytes from %s; callback %s:%u", (unsigned)run_id, (unsigned)nfiles,
                  (unsigned long long)total, peer_ip, cb_ip, (unsigned)cbport);

    /* FILE frames, in order, each file's chunks contiguous; every frame authenticated and in sequence */
    for (uint32_t i = 0; i < nfiles; i++) {
        char full[512];
        uint64_t got = 0;
        prn_sha256 s;
        uint8_t digest[32];
        FILE *out;
        snprintf(full, sizeof full, "%s/%s", partial, files[i].path);
        if (mkdirs_for(full) != 0 || (out = fopen(full, "wb")) == NULL) { reject(fd, "cannot create file"); goto fail; }
        prn_sha256_init(&s);
        while (got < files[i].size) {
            if (prn_chan_recv(&chan, &type, frame, sizeof frame, &m) != 0) { fclose(out); goto dead; }
            if (type != PRN_FILE || m < 4 || prn_get32(frame) != i || got + (m - 4) > files[i].size) { fclose(out); reject(fd, "unexpected frame during upload"); goto fail; }
            if (fwrite(frame + 4, 1, m - 4, out) != m - 4) { fclose(out); reject(fd, "write failed"); goto fail; }
            prn_sha256_update(&s, frame + 4, m - 4);
            got += m - 4;
        }
        if (fclose(out) != 0) { reject(fd, "close failed"); goto fail; }
        prn_sha256_final(&s, digest);
        if (memcmp(digest, files[i].sha, 32) != 0) { reject(fd, "sha256 mismatch"); goto fail; }
    }
    if (prn_chan_recv(&chan, &type, frame, sizeof frame, &m) != 0) goto dead;
    if (type != PRN_COMMIT || m != 0) { reject(fd, "expected COMMIT"); goto fail; }
    {
        /* A spawn that fails after exitspawn has torn the loader down cannot be reported (RPCS3: "Failed to boot
         * from exitspawn", fatal).  So the SELF header is checked here, while the loader can still reply. */
        char full[512];
        unsigned char magic[4] = { 0 };
        FILE *f;
        snprintf(full, sizeof full, "%s/%s", partial, files[0].path);
        if ((f = fopen(full, "rb")) == NULL || fread(magic, 1, 4, f) != 4) { if (f) fclose(f); reject(fd, "SELF unreadable"); goto fail; }
        fclose(f);
        static const unsigned char self_magic[4] = { 0x53, 0x43, 0x45, 0x00 };   /* certified-file header magic */
        if (memcmp(magic, self_magic, 4) != 0) { reject(fd, "file 0 is not a SELF (no certified-file header)"); goto fail; }
    }
    if (rename(partial, final_dir) != 0) { reject(fd, "cannot commit staging dir"); goto fail; }
    {
        /* the run key Kr = HMAC(Ks, label | run id) goes to the child and the returning loader through state/,
         * never through argv */
        uint8_t idb[4], kr[32];
        FILE *f;
        prn_put32(idb, run_id);
        prn_hmac4(ks, PRN_LABEL_RUN, strlen(PRN_LABEL_RUN), idb, 4, NULL, 0, NULL, 0, kr);
        f = fopen(PRN_RUNKEY_FILE, "wb");
        if (!f || fwrite(idb, 1, 4, f) != 4 || fwrite(kr, 1, 32, f) != 32 || fclose(f) != 0) {
            memset(kr, 0, sizeof kr);
            reject(fd, "cannot write the run key");
            remove_tree(final_dir);
            return 0;
        }
        memset(kr, 0, sizeof kr);
        prn_chan_send(&chan, PRN_ACCEPTED, idb, 4);
    }

    snprintf(selfpath, sizeof selfpath, "%s/%s", final_dir, files[0].path);
    snprintf(marker, sizeof marker, PRN_MARKER "%u:%s:%u:%u", (unsigned)run_id, cb_ip, (unsigned)cbport, (unsigned)timeout);
    args[0] = selfpath;
    args[argc + 1] = marker;
    args[argc + 2] = NULL;
    if (!loader_begin_spawn()) { reject(fd, "loader stopping"); goto fail; }   /* no new run once a stop was requested */
    loader_status("run %u: spawning %s", (unsigned)run_id, selfpath);
    {
        uint8_t idb[4];
        prn_put32(idb, run_id);
        prn_chan_send(&chan, PRN_SPAWNING, idb, 4);
    }
    loader_before_spawn();
    /* argv[0] is the SELF path in lv2's convention; exitspawn takes the arguments after it */
    exitspawn(selfpath, (const char **)&args[1], NULL, 0, 0, 1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);
    /* only reached when the spawn failed */
    {
        uint8_t e[4];
        prn_put32(e, (uint32_t)-1);
        prn_chan_send(&chan, PRN_SPAWN_FAILED, e, 4);
    }
    unlink(PRN_RUNKEY_FILE);
    loader_after_failed_spawn();
    loader_status("run %u: SPAWN FAILED", (unsigned)run_id);
    return 0;
fail:
    loader_status("run %u: upload failed, staging removed", (unsigned)run_id);
    remove_tree(partial);
    return 0;
dead:
    loader_status("run %u: session broke during upload (bad tag, sequence or connection); staging removed",
                  (unsigned)run_id);
    remove_tree(partial);
    return -1;
}

void loader_session(int fd, const char *peer_ip)
{
    uint8_t key[32], ns[32], nc[32], th[32], want[32], ks[32], type;
    uint8_t ch[2 + 32];
    uint32_t len;

    prn_set_timeouts(fd, PRN_AUTH_S, PRN_AUTH_S);
    if (loader_key(key) != 0) { reject_plain(fd, "not paired"); return; }
    if (loader_random(ns, sizeof ns) != 0) { reject_plain(fd, "no random numbers"); memset(key, 0, sizeof key); return; }
    ch[0] = 2; ch[1] = PRN_DIR_LOADER;
    memcpy(ch + 2, ns, 32);
    if (prn_send_frame(fd, PRN_CHALLENGE, ch, sizeof ch) != 0) { memset(key, 0, sizeof key); return; }
    /* AUTH: role, Nc, Th - one HMAC per connection, within the 10 s deadline */
    if (prn_recv_header(fd, &type, &len) != 0 || type != PRN_AUTH || len != 1 + 32 + 32 ||
        prn_recv_all(fd, frame, len) != 0 || frame[0] != PRN_DIR_HOST) {
        reject_plain(fd, "authentication expected");
        memset(key, 0, sizeof key);
        return;
    }
    memcpy(nc, frame + 1, 32);
    memcpy(th, frame + 33, 32);
    prn_hmac4(key, PRN_LABEL_HOST, strlen(PRN_LABEL_HOST), ns, 32, nc, 32, NULL, 0, want);
    if (prn_tag_diff(th, want)) { reject_plain(fd, "authentication failed"); memset(key, 0, sizeof key); return; }
    prn_hmac4(key, PRN_LABEL_LOADER, strlen(PRN_LABEL_LOADER), ns, 32, nc, 32, NULL, 0, want);
    prn_hmac4(key, PRN_LABEL_SESSION, strlen(PRN_LABEL_SESSION), ns, 32, nc, 32, NULL, 0, ks);
    memset(key, 0, sizeof key);
    if (prn_send_frame(fd, PRN_AUTH_OK, want, 32) != 0) { memset(ks, 0, sizeof ks); return; }

    memset(&chan, 0, sizeof chan);
    chan.fd = fd;
    memcpy(chan.key, ks, 32);
    chan.out_dir = PRN_DIR_LOADER;
    chan.in_dir = PRN_DIR_HOST;
    loader_status("session from %s authenticated", peer_ip);
    prn_set_timeouts(fd, PRN_UPLOAD_IDLE_S, PRN_UPLOAD_IDLE_S);
    for (;;) {
        if (prn_chan_recv(&chan, &type, frame, sizeof frame, &len) != 0) {
            loader_status("session from %s ended (closed, bad tag or out of sequence)", peer_ip);
            break;
        }
        if (type == PRN_BYE) { loader_status("session from %s ended", peer_ip); break; }
        /* the retired HEADER (no callback address) is refused before anything in it is parsed or staged */
        if (type == PRN_HEADER) { reject(fd, "old HEADER without callback address: update the host tool"); continue; }
        if (type == PRN_HEADER_CB) { if (run_upload(fd, len, peer_ip, ks) != 0) break; continue; }
        if (type >= PRN_LIST && type <= PRN_FILES_OFF) {
            if (loader_file_request(&chan, type, frame, len) != 0) break;
            continue;
        }
        loader_status("session from %s: unexpected frame type %u", peer_ip, (unsigned)type);
        break;
    }
    memset(ks, 0, sizeof ks);
    memset(&chan, 0, sizeof chan);
}
