/* The SDK version text embedded in every capture header.  It lives in its own archive member so that the archive
 * identity digest (tools/archive_digest.py) can leave it out: a version change alone must not change the digest of
 * the code a reviewed hardware profile is bound to. */
#include <stdint.h>

#ifndef PS3DK_PROF_SDK_VERSION
#define PS3DK_PROF_SDK_VERSION "unknown" /* the SDK Makefile passes the release version */
#endif

const char ps3dk_prof_sdk_version[] = PS3DK_PROF_SDK_VERSION;
const uint32_t ps3dk_prof_sdk_version_len = (uint32_t)(sizeof ps3dk_prof_sdk_version - 1);
