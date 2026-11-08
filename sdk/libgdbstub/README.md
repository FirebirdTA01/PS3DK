# Portable GDB remote session core

This directory contains the framing codec, session state machine and fixed
big-endian PPU register layout. It has no TCP server or PS3 register, memory,
breakpoint or exception backend. Those operations are supplied by callbacks.

The frozen Makefile defaults to the deliberately unimplemented RED stub. Select
the implementation explicitly when testing it:

```sh
make BUILD=/tmp/gdbstub-plain \
  CORE="gdbstub_codec.c gdbstub_session.c" test

make BUILD=/tmp/gdbstub-sanitized \
  CORE="gdbstub_codec.c gdbstub_session.c" \
  CFLAGS="-std=c99 -O2 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
  test
```

Run these commands from this directory with a C99 compiler and GNU Make. Use
distinct build directories for different compiler flags: the frozen Makefile
does not track command-line flag changes. The default fixture and register-layout
JSON files are included here, so no external worktree or evidence directory is
needed. The replay checks their pinned identities before accepting results.

To reproduce the negative implementation control, leave `CORE` at its default
and use another empty build directory. That run is expected to fail the codec
and session scenarios while retaining the valid register-layout checks.

The checked-in `red_data.h` is generated from the frozen JSON inputs. Node.js is
needed only for the explicit `regenerate` target, not for building or replaying
the tests. Do not regenerate the inputs to make a failing implementation pass.

Source provenance: the session and frozen harness come from commit
`7cba45a32`; the codec is the reviewed revision with SHA256
`e9e9ab93cb74eb8e400ea730308fe15b8accbd7596ecf049f737b016edc764c5`.
This integration adds the previously external codec and JSON inputs without
changing their bytes or the frozen test behavior.

Host replay success establishes protocol-core behavior against these fixtures.
It does not establish a usable PS3 debugger, working hardware stepping, or
successful attachment from a real GDB client.
