/* Built by tests/sdk/makefile-path-test.sh through the PSL1GHT-style
   Makefile path (ppu_rules + data_rules).  blob.bin arrives via bin2o. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ppu-types.h>
#include "blob_bin.h"

int main(void)
{
    int ok = blob_bin_size == 19 && memcmp(blob_bin, "MAKEFILE_PATH_BLOB\n", 19) == 0;
    printf("%s\n", ok ? "MAKEFILE_PATH_OK" : "MAKEFILE_PATH_BAD");
    return ok ? 0 : 1;
}
