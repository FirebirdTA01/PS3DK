#include <stdio.h>

#include <sys/process.h>
#include <mt19937.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    static const unsigned int expected[5] = {
        3499211612U,
        581869302U,
        3890346734U,
        3586334585U,
        545404204U
    };

    init_MT(5489U);
    for (unsigned int i = 0; i < 5; ++i) {
        unsigned int value = rand_int32_MT();
        printf("mt19937[%u] = %u\n", i, value);
        if (value != expected[i]) {
            return 1;
        }
    }

    return 0;
}
