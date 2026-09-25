# SDK networking

Link `-lnet_stub -lsysmodule_stub` (or `-lnet -lsysmodule`) for network
initialization and name/service lookup. BSD socket operations are owned by
`librt`, which the PPU compiler already links. Adding a resolver or initialization
call must not change which implementation supplies `socket`, `connect`, or
`socketclose`.

The combined net archive contains the original `sys_net` import records, with
private linker names for imports that overlap SDK implementations, followed by
the SDK initialization, resolver, diagnostics, and compatibility objects. `objcopy` changes
the linker names; FNIDs, import metadata, and trampoline instructions stay
unchanged. There is one import record per export, with no per-import archive
splitting. The build installs the combined archive under both supported names
in both PPU ABIs.

Public BSD socket definitions remain weak so applications can override them.
Private names prevent the raw PRX imports from overriding the SDK defaults.
The existing weak `inet_aton`/`inet_pton` fallbacks retain their prior behavior.

## Descriptor and error contract

`socket` and `accept` return descriptors tagged with `SOCKET_FD_MASK`. Use these
with the BSD family, including `read`, `write`, `close`, `select`/`socketselect`,
and `poll`/`socketpoll`. The SDK translates negative kernel network errors to
native newlib `errno`, including the reentrant read/write/close paths. The raw
PRX error location remains separate. Errors without a default-newlib equivalent
use a portable equivalent; unknown network errors become `EIO`.
Cell `0x8001xxxx` failures retain the existing Lv-2 translation. The legacy
user-limit error uses `ENOSPC`, rather than a retryable error.

`fd_set` is 128 bytes in both ABIs. `FD_SET`, `FD_CLR`, and `FD_ISSET` remove the
tag, evaluate arguments once, and bound the bit index. `select` accepts either
tagged maximum descriptor plus one or raw maximum plus one; out-of-range
`nfds` and invalid timeouts fail with `EINVAL`. Only sockets are selectable.
`poll` preserves caller descriptors, ignores negative entries, and reports
`POLLNVAL` for non-socket descriptors.

The compatibility `netSocket`/`netAccept`/`netSend` family explicitly retains
raw PRX descriptors and `net_errno`. It rejects tagged descriptors. Do not mix
these descriptors with BSD calls. Prefer the BSD family in new code.

Canonical `sys_net_get_sockinfo`/`sys_net_get_sockinfo_ex` accept tagged input
and preserve raw values, including the `-1` all-sockets sentinel. They return
the raw count/error and tag nonnegative descriptors only in returned entries;
negative descriptors such as TIME_WAIT sentinels stay negative.
`sys_net_abort_socket` strips the input tag and retains the raw result.
These controls retain `sys_net_errno`, not BSD `errno`. Legacy `netGetSockInfo`
and `netAbortSocket` retain raw descriptors.

## Data conversion

The SDK explicitly packs Lv-2 message headers and vectors and translates native
timeouts to the two-64-bit-field kernel format. Public `ATTRIBUTE_PRXPTR`
fields retain their existing 32-bit storage even in LP64. No cast of a native
`msghdr`, `iovec`, or `timeval` is used as a kernel layout.

`gethostbyname` and `gethostbyaddr` convert firmware's 32-bit pointer fields to
native `hostent` pointers, copying names, alias lists and addresses. Results
belong to the calling thread, last until its next host lookup, and are freed
at thread exit. Lists are terminated. A list with 1024 non-null entries,
an unterminated 1024-byte name, or an invalid address length is rejected with
`NO_RECOVERY`/`EOVERFLOW`. `h_errno` remains a real process-global `int` for
legacy source/binary compatibility; concurrent lookups can overwrite that
error value. `sys_net_h_errno` exposes the separate raw firmware location.

Firmware has no service database. `getservbyport` and `getservbyname` use an SDK
table for ftp-data, ftp, ssh, telnet, smtp, domain (TCP and UDP), http, pop3,
ntp, imap, https, submission, imaps and pop3s. Unknown service/protocol pairs
return null. `servent.s_port` is an `int` containing a network-order port;
its pointers have native width. Service results are thread-local.

`netInitialize` loads the network module and initializes a 128 KiB buffer.
It is serialized and idempotent: repeated successful calls still need only
one `netDeinitialize`, which finalizes networking and unloads the module.
Initialization failure unloads the module; module-load failure does not call
the network import. Direct `sys_net_initialize_network_ex` callers own their
module load, buffer, and lifecycle; do not interleave the two lifecycle APIs.

## Validation

- `tests/sdk/libnet-host-test.py` executes production wrappers against syscall
  and PRX fixtures, with independent wire layouts and output assertions.
- `tests/sdk/libnet-link-test.py --ps3dev PREFIX --output DIR` writes final
  ELFs, link maps and hashes for 80 both-ABI/order/group/whole-archive cases.
- `tests/sdk/libnet-override-test.py` proves application definitions still win.
- `tests/sdk/libnet-compat-test.py --ps3dev PREFIX --red-control` checks legacy
  exports and requires failure when a temporary archive loses `netSocket`.
- `tests/regression/libnet` tests actual UDP/TCP loopback, options, tagged
  select/poll, read/write/close, representative errors, and init/finalize.

RPCS3 HLE does not implement firmware DNS resolution or Lv-2 `recvmsg` in the
version used for this gate. Passing fixtures prove SDK conversion behavior;
they do not prove those firmware paths on hardware. Report those results
separately from the real loopback test.

Windows RPCS3 timeout readback is a separate limitation: both ABI probes
observed success, `errno=0`, length 16, and `{2000,0}` after setting `{2,500}`.
Its `lv2_socket_native.cpp:721-725` writes Windows DWORD milliseconds, while
lines 595-601 read the result as a native timeval. The regression reports that
exact value as `LIBNET_EMULATOR_LIMIT`, not a timeout readback pass. Setting
timeouts, supported socket-option readback, and all transfers remain
strict gates. Other malformed timeout values fail. Host conversion controls
still assert both timeout fields exactly.

RPCS3's same SOL_SOCKET getter switch (lines 388-479) does not implement
SO_TYPE. The probe logs its return, errno, length and value; exactly
`-1/EINVAL` is a separate unimplemented row, while success must report the
requested socket type. REUSEADDR, ERROR and transfers stay strict. RPCS3's
module and network initialization/finalization are HLE no-ops, so lifecycle
ordering and failure handling are established by host controls, not boots.

RPCS3 also forwards MSG_WAITALL to its nonblocking Winsock sockets, which
reject that combination. Only SDK `-1/EOPNOTSUPP`, corroborated by raw syscall
`-45` and a host translation control, permits a separately reported limitation.
The probe then receives with flags zero, bounded by payload size and iteration
count, and still requires every byte. EOF/error/short total fails the gate.
