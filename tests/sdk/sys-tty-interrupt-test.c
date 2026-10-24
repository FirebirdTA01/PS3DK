/* tests/sdk/sys-tty-interrupt-test.c: the SDK-owned <sys/tty.h> and <sys/interrupt.h>.  Every reference and
 * PSL1GHT name is used (compiled under -Wall -Wextra -Werror in C and C++, both ABIs), and two probes pass
 * 32-bit addresses with the high bit set so the driver can check, by value, that ILP32 pointers reach the
 * system call zero-extended (r4 and r6 at the sc instruction). */
/* -DLV2_FIRST / -DLV2_AFTER: PSL1GHT's <lv2/interrupt.h> before or after ours (both must compile). */
#if defined(LV2_FIRST)
# include <lv2/interrupt.h>
#endif
#include <sys/tty.h>
#include <sys/interrupt.h>
#if defined(LV2_AFTER)
# include <lv2/interrupt.h>
#endif

#if defined(__cplusplus)
# define CHECK(c, m) static_assert(c, m)
#else
# define CHECK(c, m) _Static_assert(c, m)
#endif
CHECK(SYS_TTYP_MAX == 16 && SYS_TTYP_PPU_STDOUT == 0 && SYS_TTYP_PPU_STDERR == 1 && SYS_TTYP_SPU_STDOUT == 2 &&
      SYS_TTYP_USER1 == 3 && SYS_TTYP_USER13 == 15, "TTY channels");
CHECK(SYS_HW_THREAD_ANY == 0xFFFFFFFEu && SYS_HW_THREAD_INVALID == 0xFFFFFFFFu &&
      SYS_INTERRUPT_TAG_ID_INVALID == 0xFFFFFFFFu, "interrupt constants");

int tty_probe(void)
{
    return sys_tty_write(SYS_TTYP_PPU_STDOUT, (const void *)0x80000004u, 4, (unsigned int *)0x80000010u);
}

int tty_read_probe(void)
{
    return sys_tty_read(SYS_TTYP_PPU_STDIN, (void *)0x80000004u, 4, (unsigned int *)0x80000010u);
}

int every_name(unsigned int *n, sys_interrupt_tag_t *tag, sys_interrupt_thread_handle_t *ih, u64 *tls)
{
    char buf[4] = { 'o', 'k', '\n', 0 };
    u32 w = 0, r = 0;
    int rc = sys_tty_write(SYS_TTYP_USER1, buf, 3, n) + sysTtyWrite(SYS_TTYP_USER2, buf, 3, &w) +
             sys_tty_read(SYS_TTYP_PPU_STDIN, buf, 1, n) + sysTtyRead(SYS_TTYP_PPU_STDIN, buf, 1, &r);
    rc += sys_interrupt_tag_create(tag, 0, SYS_HW_THREAD_ANY) + sysInterruptTagCreate(tag, 0, SYS_HW_THREAD_ANY);
    rc += sys_interrupt_thread_establish(ih, *tag, 0, 0) + sysInterruptThreadEstablish(ih, *tag, 0, 0);
    rc += _sys_interrupt_thread_establish(ih, *tag, 0, 0, 0);
    sys_interrupt_thread_eoi();
    sysInterruptThreadEOI();
    rc += _sys_interrupt_thread_disestablish(*ih, tls) + _sysInterruptThreadDisestablish(*ih, tls);
    rc += sys_interrupt_tag_destroy(*tag) + sysInterruptTagDestroy(*tag);
    /* the library call under both names (liblv2_stub), distinct from the raw system call above */
    rc += sys_interrupt_thread_disestablish(*ih) + sysInterruptThreadDisestablish(*ih);
    return rc;
}
