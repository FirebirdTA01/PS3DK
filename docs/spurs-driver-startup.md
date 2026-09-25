# SPURS driver startup selection

The SPU driver distinguishes task, job and initialized-job links. These modes
must select their own startup objects and layout, while ordinary SPU programs
retain the existing `main` startup. Combining SPURS modes, or combining a
SPURS mode with shared or relocatable linking, is unsupported and must fail
with a diagnostic. Ordinary relocatable linking remains available.

| Mode | Startup objects | Internal service archive | Default layout |
|---|---|---|---|
| `-mspurs-task` | `spurs_task.o` | `libspurs_task_runtime.a` | `spurs_task.ld`, entry at 0x3000 |
| `-mspurs-job` | `job_start.o` | `libspurs_job_runtime.a` | `spurs_job.ld`, entry at 0x10 |
| `-mspurs-job-initialize` | `job_start_w_crt.o`, `job_crt.o` | `libspurs_jq_runtime.a` | `spurs_job.ld`, entry at 0x10 |

The internal archives exclude startup symbols. `-nostartfiles` removes startup
objects without silently retrieving them from a service archive. `-nodefaultlibs`
suppresses default service libraries; `-nostdlib` suppresses both. An explicit
linker script, passed with `-T` or `-Wl,-T`, takes precedence over the default.
Application objects precede runtime archives. A legacy entry hidden in an
application archive still follows normal archive extraction rules.

Existing `libspurs_task.a`, `libspurs_job.a` and `libspurs_jq.a` retain their
startup members for explicit manual links. They are not aliases of the internal
service-only archives. Force-loading a complete manual archive alongside driver
startup can define a startup symbol twice.

An initialized job can define `cellSpursJobMain2` directly. Otherwise, a separate
strong member adapts that entry to application-defined `cellSpursJobQueueMain`.
A direct definition wins; if neither entry is supplied, linking fails. This
split does not supply constructors, destructors, atexit registrations, job DMA
setup or scheduling semantics missing from the existing runtime.

These choices intentionally differ from the reference driver in limited ways.
The reference accepts conflicting mode/shared/relocatable combinations at the
driver stage; those combinations are outside our supported contract. It selects
its own start/end objects and address options. Our independently authored linker
scripts and runtime objects have different section and startup requirements.
We do not ship dummy counterparts for reference constructor or fixup objects.
Canonical common SPU `libspurs.a` packaging is a separate change.

Verification distinguishes driver command traces, actual link/ownership checks,
rebuilt compiler integration, and runtime execution. A private specs overlay
only demonstrates candidate selection behavior; it does not update an installed
compiler or establish runtime correctness.

## Canonical SPU service archive

SPU `-lspurs` selects the canonical `libspurs.a` in the SPU target library
directory (also installed to the SDK's `spu/lib`). It contains existing module
and task services, semaphore operations and their signalling dependencies.
It contains no startup object and no `cellSpursMain` entry adapter. The full
`libspurs_task.a` and internal `libspurs_task_runtime.a` retain their existing
members; the job and initialized-job archives are unchanged. PPU `libspurs.a`
continues to name the separate PPU import archive.

The reference SPU `libspurs.a` includes startup members, including `main.o`.
PS3DK deliberately differs: driver patch 0006 selects the mode-specific startup
object and specialized service archive explicitly. Adding task startup back to
the canonical archive would blur that ownership and can conflict with job
startup. Use the selected driver mode or the documented manual startup path.

Archive order follows ordinary static linking. When both common and task
runtime archives provide a service, the first archive that satisfies its
unresolved reference supplies it; the other archive does not extract another
definition. Task entry adaptation remains in the specialized task runtime.

These are existing implementations with existing context requirements, not new
runtime capabilities. Task getters, task exit and blocking waits require the
task control state; module getters and dispatch require a SPURS workload.
An ordinary SPU program can link the archive for ownership tests but cannot
call those services safely without their required SPURS environment. Compile
and link gates do not establish runtime, constructor or firmware behavior.

## Job content identity

Both job startups define the global `__SPU_GUID` symbol at the start of a
16-byte AX PROGBITS `.SpuGUID` section. The patched linker fills that section
after relocation. A final link opts in only by defining this symbol; ordinary
SPU links without it retain stock linker behavior. Relocatable links and BFD
copy/strip operations do not regenerate the identity. A malformed reservation
is a link error. A sample must not add a second placeholder section.

The linker also rejects conflicting SPURS program-type flags and relocatable
SPURS links. This enforces the same policy when options arrive through `-Wl`
or `-Xlinker`. Repeating the same mode is allowed; ordinary `-r` links remain
supported. Stock SPU ld already rejects shared linking.

PS3DK uses an independent 64-bit content identity; it does not reproduce Sony's
digest. The digest is SHA-1 truncated to its first eight bytes. Its input is:

1. The bytes `PS3DK-SPU-GUID`, a zero byte and a byte with value 1.
2. Entry address, ELF flags and record count, each as a big-endian 32-bit value.
3. Each allocated section except `.SpuGUID` and NOTE sections, ordered by address
   then section name. Each record contains big-endian 32-bit address, section
   type, ELF section flags, size and name length, followed by the name bytes
   without a terminator and the final initialized section bytes. NOBITS records
   include metadata but no data bytes.

This covers startup, code, read-only and initialized data, plus BSS extent.
Excluding NOTE sections avoids a generated build-id dependency cycle. Debug
information, file offsets, filenames outside section names and timestamps are
excluded. The four big-endian 16-bit pieces of the resulting identity become
ILA instructions targeting register 2, with immediate `(piece << 2) | index`
for indices 0 through 3.

The GUID identifies a program for consumers of its embedded identity, including
diagnostics and tracing. This change does not establish firmware cache lookup
behavior. Tests distinguish identical rebuilds, one-instruction changes and
one-data-byte changes, and reject a stale or malformed identity. A 64-bit
identity is probabilistic and is not an authentication value.
