#include "shared.h"

int mkpath_bump(void)
{
    return ++mkpath_shared_counter;
}
