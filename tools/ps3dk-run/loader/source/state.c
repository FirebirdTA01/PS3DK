/*
 * ps3dk-loader: pairing key and persistent state, all under PRN_STATE_DIR, which no file-service root
 * contains.  The key is 32 random bytes generated on the host and imported once from a USB stick (out of band); it is
 * never printed, put in argv or served - only its fingerprint is shown.
 */
#include "loader.h"
#include "../../common/prn_sha256.h"
#include "../../common/ps3dk_run_proto.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/random_number.h>
#include <sys/stat.h>
#include <unistd.h>

static char fp[24] = "(not paired)";

int loader_random(void *p, unsigned n)
{
    return sys_get_random_number(p, n) == 0 ? 0 : -1;
}

static void make_fingerprint(const uint8_t key[32])
{
    prn_sha256 s;
    uint8_t d[32];
    prn_sha256_init(&s);
    prn_sha256_update(&s, key, 32);
    prn_sha256_final(&s, d);
    for (int i = 0; i < 8; i++) snprintf(fp + 2 * i, 3, "%02x", d[i]);
}

int loader_key(uint8_t key[32])
{
    FILE *f = fopen(PRN_KEY_FILE, "rb");
    int ok;
    if (!f) { strcpy(fp, "(not paired)"); return -1; }
    ok = fread(key, 1, 32, f) == 32 && fgetc(f) == EOF;
    fclose(f);
    if (!ok) { strcpy(fp, "(bad key file)"); return -1; }
    make_fingerprint(key);
    return 0;
}

const char *loader_fingerprint(void) { return fp; }

int loader_import_key(void)
{
    static const char *const src[] = { "/dev_usb000/PS3DKRUN.KEY", "/dev_usb001/PS3DKRUN.KEY" };
    uint8_t key[32];
    for (unsigned i = 0; i < sizeof src / sizeof src[0]; i++) {
        FILE *f = fopen(src[i], "rb");
        int ok;
        if (!f) continue;
        ok = fread(key, 1, 32, f) == 32 && fgetc(f) == EOF;
        fclose(f);
        if (!ok) { loader_status("key import: %s is not exactly 32 bytes", src[i]); return -1; }
        mkdir(PRN_STATE_DIR, 0700);
        f = fopen(PRN_KEY_FILE ".new", "wb");
        if (!f || fwrite(key, 1, 32, f) != 32) { if (f) fclose(f); loader_status("key import: cannot write state"); return -1; }
        fclose(f);
        unlink(PRN_KEY_FILE);
        if (rename(PRN_KEY_FILE ".new", PRN_KEY_FILE) != 0) { loader_status("key import: cannot store key"); return -1; }
        make_fingerprint(key);
        memset(key, 0, sizeof key);
        loader_status("key imported from %s - fingerprint %s (remove the file from the stick)", src[i], fp);
        return 0;
    }
    loader_status("key import: no PS3DKRUN.KEY on /dev_usb000 or /dev_usb001");
    return -1;
}

void loader_revoke_key(void)
{
    unlink(PRN_KEY_FILE);
    unlink(PRN_RUNKEY_FILE);
    strcpy(fp, "(not paired)");
    loader_status("key revoked: every session is rejected until a key is imported again");
}

void loader_state_init(int returning)
{
    struct stat st;
    mkdir(PRN_STATE_DIR, 0700);
    if (returning && stat(PRN_FILES_ON, &st) == 0) {
        loader_set_files(1, "restored after the run");
    } else {
        unlink(PRN_FILES_ON);
        loader_set_files(0, returning ? "was off before the run" : "cold start");
    }
}
