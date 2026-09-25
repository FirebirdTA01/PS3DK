/* SPDX-License-Identifier: BSD-2-Clause
 * Lv-2 message slots are 64 bits with zero high words and 32-bit EAs.
 * Explicit fields avoid ILP32/native-pointer layout coincidences.
 */
#ifndef PS3DK_NET_WIRE_H
#define PS3DK_NET_WIRE_H
#include <stdint.h>
#include <stddef.h>
struct net_wire_iovec { uint32_t zero_base, base, zero_len, len; };
struct net_wire_msghdr {
    uint32_t zero_name, name, namelen, pad_name;
    uint32_t zero_iov, iov;
    int32_t iovlen;
    uint32_t pad_iov, zero_control, control, controllen;
    int32_t flags;
};
_Static_assert(sizeof(struct net_wire_iovec) == 16, "Lv-2 iovec size");
_Static_assert(sizeof(struct net_wire_msghdr) == 48, "Lv-2 msghdr size");
_Static_assert(offsetof(struct net_wire_msghdr, control) == 36, "Lv-2 control EA");
#endif
