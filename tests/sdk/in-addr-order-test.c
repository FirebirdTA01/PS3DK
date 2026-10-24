/* tests/sdk/in-addr-order-test.c: <cell/libnetctl.h> (and its <netex/libnetctl.h> forwarder) with
 * <netinet/in.h>, in the order chosen by -DORDER_*.  libnetctl.h used to declare its own struct in_addr
 * unless netinet/in.h came first, so libnetctl.h then netinet/in.h redefined the struct. */
#if defined(ORDER_NETINET_FIRST)
# include <netinet/in.h>
# include <cell/libnetctl.h>
#elif defined(ORDER_LIBNETCTL_FIRST)
# include <cell/libnetctl.h>
# include <netinet/in.h>
#elif defined(ORDER_NETEX_FIRST)
# include <netex/libnetctl.h>
# include <netinet/in.h>
#else
# include <cell/libnetctl.h>
#endif

/* The struct a NAT-info query fills and the socket address use the same in_addr. */
int in_addr_probe(const struct CellNetCtlNatInfo *nat, struct sockaddr_in *sin)
{
    sin->sin_addr = nat->mapped_addr;
    return sizeof(struct in_addr) == 4 && sin->sin_addr.s_addr == nat->mapped_addr.s_addr;
}
