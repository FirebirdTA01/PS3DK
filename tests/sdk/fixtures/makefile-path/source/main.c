/* Built by tests/sdk/makefile-path-test.sh through the PSL1GHT-style
   Makefile path (ppu_rules + data_rules).  blob.bin arrives via bin2o;
   shared.h defines a variable that second.c sees too (needs -fcommon). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ppu-types.h>
#include "blob_bin.h"
#include "shared.h"

int main(void)
{
    int ok = blob_bin_size == 19 && memcmp(blob_bin, "MAKEFILE_PATH_BLOB\n", 19) == 0;
    ok = ok && mkpath_bump() == 1 && mkpath_shared_counter == 1;
    printf("%s\n", ok ? "MAKEFILE_PATH_OK" : "MAKEFILE_PATH_BAD");
    return ok ? 0 : 1;
}
