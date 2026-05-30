#include <stdio.h>

#include <sys/process.h>
#include <cell/ovis.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    static const unsigned char minimal_elf[] = {
        0x7f, 'E', 'L', 'F',
        0x02, 0x02, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };

    int size = cellOvisGetOverlayTableSize((const char *)minimal_elf);
    printf("cellOvisGetOverlayTableSize -> %d\n", size);
    return 0;
}
