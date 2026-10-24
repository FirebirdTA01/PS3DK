/* tests/sdk/sys-file-test.c: the SDK-owned <sys/file.h> (and the <sys/lv2_fs_ext.h> forwarder).  Every wrapper
 * compiles under -Wall -Wextra -Werror in C and C++, both ABIs; the *_probe functions pass 32-bit addresses with
 * the high bit set so the driver can read, at each sc, that every pointer argument reached the kernel
 * zero-extended (the 23 wrappers widen through uintptr_t). */
#if defined(VIA_FORWARDER)
# include <sys/lv2_fs_ext.h>
#else
# include <sys/file.h>
#endif

#define P(a) ((void *)(a))

int open_probe(void)   { return sysLv2FsOpen((const char *)P(0x80000004u), 0, (s32 *)P(0x80000010u), 0, P(0x80000020u), 0); }
int read_probe(void)   { return sysLv2FsRead(3, P(0x80000004u), 16, (u64 *)P(0x80000010u)); }
int stat_probe(void)   { return sysLv2FsStat((const char *)P(0x80000004u), (sysFSStat *)P(0x80000010u)); }
int rename_probe(void) { return sysLv2FsRename((const char *)P(0x80000004u), (const char *)P(0x80000010u)); }
int lseek_probe(void)  { return sysLv2FsLSeek64(3, 0, 0, (u64 *)P(0x80000004u)); }
int chown_probe(void)  { return sysLv2FsChown((const char *)P(0x80000004u), 1, 2); }
int mount_probe(void)  { return sysLv2FsMount((const char *)P(0x80000004u), (const char *)P(0x80000010u),
                                              (const char *)P(0x80000020u), 1); }

int every_name(const char *path, s32 fd, sysFSStat *st, sysFSDirent *de, const sysFSUtimbuf *ut)
{
    s32 nfd = 0; u64 n = 0; char buf[8] = { 0 };
    return sysLv2FsOpen(path, 0, &nfd, 0, 0, 0) + sysLv2FsClose(fd) + sysLv2FsRead(fd, buf, 1, &n) +
           sysLv2FsWrite(fd, buf, 1, &n) + sysLv2FsStat(path, st) + sysLv2FsFStat(fd, st) + sysLv2FsChmod(path, 0) +
           sysLv2FsOpenDir(path, &nfd) + sysLv2FsReadDir(fd, de, &n) + sysLv2FsCloseDir(fd) + sysLv2FsMkdir(path, 0) +
           sysLv2FsRename(path, path) + sysLv2FsRmdir(path) + sysLv2FsLSeek64(fd, 0, 0, &n) + sysLv2FsUnlink(path) +
           sysLv2FsFsync(fd) + sysLv2FsTruncate(path, 0) + sysLv2FsFtruncate(fd, 0) + sysLv2FsUtime(path, ut) +
           sysLv2FsLink(path, path) + sysLv2FsChown(path, 0, 0) + sysLv2FsMount(path, path, path, 0) +
           sysLv2FsUnmount(path);
}
