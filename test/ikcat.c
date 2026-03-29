/* ikcat.c — standalone cat ELF for IronKernel iksh */
#include <stdio.h>
#include <ironkernel.h>

static uint8_t buf[8192];

int main(void)
{
    /* Read from stdin (pipe) and print */
    uint64_t n;
    while ((n = ik_read((char*)buf, sizeof(buf)-1)) > 0) {
        buf[n] = 0;
        printf("%s", (char*)buf);
    }
    return 0;
}
