/* Compile-only guard for indirect SYS_PROCESS_PARAM visibility. */

#if defined(TEST_INCLUDE_CELL_SPURS)
# include <cell/spurs.h>
#elif defined(TEST_INCLUDE_SYS_DBG)
# include <sys/dbg.h>
#else
# error "Specify TEST_INCLUDE_CELL_SPURS or TEST_INCLUDE_SYS_DBG"
#endif

/* Exercise the macro through each public entry header alone. */
SYS_PROCESS_PARAM(1001, 0x10000)

int main(void)
{
    return 0;
}
