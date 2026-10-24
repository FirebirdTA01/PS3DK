/* sys-tty: the SDK-owned <sys/tty.h> writes to the TTY through lv2 system call 403 under both spellings.
 * Each write must return 0 and report every byte written, and its text must appear in the TTY log: the
 * manifest row requires both marker lines, in order, before SYS_TTY_OK. */
#include <stdio.h>
#include <sys/tty.h>

int main(void)
{
    static const char ref[] = "SYS_TTY_ROW reference-name write\n";
    static const char old[] = "SYS_TTY_ROW psl1ght-name write\n";
    unsigned int written = 0;
    u32 written2 = 0;
    int fails = 0;
    int rc = sys_tty_write(SYS_TTYP_PPU_STDOUT, ref, sizeof ref - 1, &written);
    if (rc != 0 || written != sizeof ref - 1) {
        printf("SYS_TTY_FAIL sys_tty_write rc=%d written=%u of %u\n", rc, written, (unsigned)(sizeof ref - 1));
        fails++;
    }
    rc = sysTtyWrite(SYS_TTYP_PPU_STDOUT, old, sizeof old - 1, &written2);
    if (rc != 0 || written2 != sizeof old - 1) {
        printf("SYS_TTY_FAIL sysTtyWrite rc=%d written=%u of %u\n", rc, (unsigned)written2, (unsigned)(sizeof old - 1));
        fails++;
    }
    if (fails == 0)
        printf("SYS_TTY_OK\n");
    fflush(stdout);
    return 0;
}
