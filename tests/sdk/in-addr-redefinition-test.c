/*
 * in-addr-redefinition-test.c — Verify struct in_addr declaration compatibility
 *
 * Verifies that <cell/libnetctl.h>, <netex/libnetctl.h>, and <netinet/in.h>
 * can be included in any order without "redefinition of struct in_addr" errors,
 * across all C and C++ language standards.
 */

#if defined(TEST_ORDER_NETINET_THEN_LIBNETCTL)
# include <netinet/in.h>
# include <cell/libnetctl.h>
#elif defined(TEST_ORDER_LIBNETCTL_THEN_NETINET)
# include <cell/libnetctl.h>
# include <netinet/in.h>
#elif defined(TEST_ORDER_NETEX_THEN_NETINET)
# include <netex/libnetctl.h>
# include <netinet/in.h>
#elif defined(TEST_ORDER_NETINET_THEN_NETEX)
# include <netinet/in.h>
# include <netex/libnetctl.h>
#else
# include <netinet/in.h>
# include <cell/libnetctl.h>
# include <netex/libnetctl.h>
#endif

int check_in_addr(void)
{
    struct in_addr addr;
    addr.s_addr = 0x7f000001; /* 127.0.0.1 */
    return (addr.s_addr == 0x7f000001 && sizeof(struct in_addr) == 4) ? 0 : 1;
}

int check_netctl_info(void)
{
    union CellNetCtlInfo info;
    struct in_addr addr;
    addr.s_addr = 0x0a000001;
    info.device = 1;
    return (info.device == 1 && addr.s_addr == 0x0a000001) ? 0 : 1;
}
