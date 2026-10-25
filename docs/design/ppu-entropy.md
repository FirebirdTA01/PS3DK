# PPU entropy provider

PPU newlib obtains `getentropy` bytes from the sysPrxForUser random-number
service (`sys_get_random_number`, NID `0x71a8472a`). The normal SDK link group
provides that import through `liblv2_stub`. The declaration and calling
convention match the public PSL1GHT `sysGetRandomNumber` interface. No SDK
private source is used.

The newlib interface accepts at most 256 bytes. A zero-length request succeeds
without calling the service; an oversized request fails with `EIO`, and a null
buffer with nonzero length fails with `EFAULT`. A nonzero service result fails
the request. LV2 errno values are preserved; unexpected return values become
`EIO`. There is no clock seed, deterministic substitute, or success fallback.
Buffer contents after an error must not be used.

The public `getentropy` and reentrant `_getentropy_r` entries live in separate
archive members. This permits newlib's public connector to resolve to the
reentrant entry when libc precedes libsysbase, without introducing a duplicate
public definition. The reentrant entry changes only the supplied reentrancy
record's errno on failure.

For PPU GCC 16, libstdc++ selects the getentropy backend explicitly. Its
configuration no longer depends on whether a bootstrap stage can link an
executable with the SDK linker script and import libraries. Both `configure.ac`
and generated `configure` carry this target-specific choice. Other targets keep
their upstream probes. A failed provider causes `std::random_device` to report
failure, rather than silently substituting a predictable source.

Newlib's `arc4random` remains available as a separate API. On PPU, failure to
seed it calls `abort()`: the generic `raise(SIGKILL)` path is insufficient when
the target's signal syscall can return an error. It must never proceed with
the zero-initialized seed after an entropy failure.

Host tests exercise bounds, error propagation, reentrancy and both archive
orders. Built-library tests check configuration and actual provider symbols in
both ABIs. Target probes separately test successful reads, random_device,
provider-error propagation, and arc4random's expected abort. Sample variation
is only a smoke check; it does not certify entropy quality or hardware security.
