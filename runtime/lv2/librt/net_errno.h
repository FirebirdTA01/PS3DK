/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PS3DK_NET_ERRNO_H
#define PS3DK_NET_ERRNO_H
#include <errno.h>
#include <sys/reent.h>
#include <sys/lv2errno.h>
#include <stdint.h>

/* sys_net uses BSD errno numbers, NOT Cell 0x8001xxxx return codes and
 * NOT newlib's errno numbering. Values are ABI facts; see RPCS3 sys_net.h. */
static inline int net_error(int error)
{
    switch (error) {
    case 0: return 0;
    case 1: return EPERM; case 2: return ENOENT; case 3: return ESRCH;
    case 4: return EINTR; case 5: return EIO; case 6: return ENXIO;
    case 7: return E2BIG; case 8: return ENOEXEC; case 9: return EBADF;
    case 10: return ECHILD; case 11: return EDEADLK; case 12: return ENOMEM;
    case 13: return EACCES; case 14: return EFAULT; case 16: return EBUSY;
    case 17: return EEXIST; case 18: return EXDEV; case 19: return ENODEV;
    case 20: return ENOTDIR; case 21: return EISDIR; case 22: return EINVAL;
    case 23: return ENFILE; case 24: return EMFILE; case 25: return ENOTTY;
    case 26: return ETXTBSY; case 27: return EFBIG; case 28: return ENOSPC;
    case 29: return ESPIPE; case 30: return EROFS; case 31: return EMLINK;
    case 32: return EPIPE; case 33: return EDOM; case 34: return ERANGE;
    case 35: return EAGAIN; case 36: return EINPROGRESS; case 37: return EALREADY;
    case 38: return ENOTSOCK; case 39: return EDESTADDRREQ; case 40: return EMSGSIZE;
    case 41: return EPROTOTYPE; case 42: return ENOPROTOOPT;
    case 43: return EPROTONOSUPPORT; case 44: return EPROTONOSUPPORT;
    case 45: return EOPNOTSUPP; case 46: return EPFNOSUPPORT;
    case 47: return EAFNOSUPPORT; case 48: return EADDRINUSE;
    case 49: return EADDRNOTAVAIL; case 50: return ENETDOWN;
    case 51: return ENETUNREACH; case 52: return ENETRESET;
    case 53: return ECONNABORTED; case 54: return ECONNRESET;
    case 55: return ENOBUFS; case 56: return EISCONN; case 57: return ENOTCONN;
    case 58: return EPIPE; case 59: return ETOOMANYREFS;
    case 60: return ETIMEDOUT; case 61: return ECONNREFUSED;
    case 62: return ELOOP; case 63: return ENAMETOOLONG;
    case 64: return EHOSTDOWN; case 65: return EHOSTUNREACH;
    case 66: return ENOTEMPTY; case 68: return ENOSPC; case 69: return EDQUOT;
    case 70: return ESTALE; case 71: return EIO; case 77: return ENOLCK;
    case 78: return ENOSYS; case 82: return EIDRM; case 83: return ENOMSG;
    case 84: return EOVERFLOW; case 85: return EILSEQ; case 86: return ENOTSUP;
    case 87: return ECANCELED; case 88: return EBADMSG; case 89: return ENODATA;
    case 90: return ENOSR; case 91: return ENOSTR; case 92: return ETIME;
    default: return EIO;
    }
}
static inline int net_return_error(int result)
{
    if (((uint32_t)result & 0xffff0000u) == 0x80010000u) return lv2error(result);
    return result >= -92 ? net_error(-result) : EIO;
}
static inline int net_result(int result)
{
    if (result >= 0) return result;
    /* Avoid signed overflow on a malformed kernel return. */
    errno = net_return_error(result);
    return -1;
}
static inline int net_result_r(struct _reent *r, int result)
{
    if (result >= 0) return result;
    r->_errno = net_return_error(result);
    return -1;
}
#endif
