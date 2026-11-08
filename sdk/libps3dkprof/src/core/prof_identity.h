/* The PROFILE record's build identity (docs/local/prof-viewer/profile-format-spec.md, gate B2p, draft r1).
 *
 * Only builds with PS3DK_PROF_WITH_PROFILE use it (the opt-in archive `make ext-ilp32-profile`, and host tests).  The
 * one definition lives in the GENERATED archive member prof_identity.o (tools/archive_digest.py emit), which holds
 * constants only and is excluded from its own digest: runtime_digest is ARCHIVE-DIGEST-V1 over every OTHER member of
 * the archive (spec 4).  The marker lets an independent check find the struct in the object bytes. */
#ifndef PS3DK_PROF_IDENTITY_H
#define PS3DK_PROF_IDENTITY_H

#include <stdint.h>

#define PS3DK_PROF_IDENTITY_MARKER "ps3dkprof.ident1" /* exactly 16 bytes, no NUL stored */

typedef struct ps3dk_prof__identity_t {
    char marker[16];
    uint8_t digest[32];    /* ARCHIVE-DIGEST-V1 of the archive's other members */
    uint32_t premise_bits; /* spec 3.2 */
    uint32_t kinds;        /* bit 0 RSX, bit 1 SPU */
    uint8_t id_len;        /* 1..32 */
    char id[32];           /* A-Z 0-9 -, first byte a letter; not NUL-terminated when 32 long */
} ps3dk_prof__identity_t;

extern const ps3dk_prof__identity_t ps3dk_prof__identity;

#endif
