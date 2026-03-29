/* IronKernel iklibc demo — test/demo.c
   Exercises every part of iklibc: crt0, syscalls, string, stdio, stdlib.
   Build: make test/demo.elf  then  inject into disk.img */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ironkernel.h>

/* ── Static variable — will be 0 only if crt0 zeroed .bss correctly ── */
static int bss_check = 0;

int main(void)
{
    printf("================================\n");
    printf("  IronKernel iklibc v1.0 Demo\n");
    printf("================================\n\n");

    /* ── 1. BSS zeroing proof ────────────────────────────────────── */
    bss_check++;
    printf("[crt0]   bss_check = %d  (expected 1)\n", bss_check);

    /* ── 2. String library ───────────────────────────────────────── */
    printf("\n[string.h]\n");

    const char *s = "IronKernel";
    printf("  strlen(\"%s\") = %d\n", s, (int)strlen(s));

    char buf[64];
    strcpy(buf, "Hello,");
    strcat(buf, " world!");
    printf("  strcpy+strcat  = \"%s\"\n", buf);

    printf("  strcmp same    = %d\n", strcmp("abc", "abc"));
    printf("  strcmp diff    = %d\n", strcmp("abc", "abd"));

    char ncpy[16];
    strncpy(ncpy, "IronKernel", 4);
    ncpy[4] = '\0';
    printf("  strncpy(4)     = \"%s\"\n", ncpy);

    printf("  strchr('K')    = \"%s\"\n", strchr(s, 'K'));

    /* ── 3. Memory functions ─────────────────────────────────────── */
    printf("\n[string.h / mem]\n");

    char mbuf[16];
    memset(mbuf, 'X', 8);
    mbuf[8] = '\0';
    printf("  memset('X',8)  = \"%s\"\n", mbuf);

    char src[8] = "ABCDEFG";
    char dst[8];
    memcpy(dst, src, 7); dst[7] = '\0';
    printf("  memcpy         = \"%s\"\n", dst);

    /* ── 4. stdlib: atoi / itoa / utoa ──────────────────────────── */
    printf("\n[stdlib.h]\n");

    int n = atoi("1337");
    printf("  atoi(\"1337\")  = %d\n", n);
    printf("  atoi(\"-42\")   = %d\n", atoi("-42"));

    char ibuf[32];
    itoa(n * 2, ibuf, 10);
    printf("  itoa(1337*2)   = %s\n", ibuf);

    char ubuf[32];
    utoa(255, ubuf, 16);
    printf("  utoa(255,16)   = %s  (expect ff)\n", ubuf);

    /* ── 5. malloc / free ────────────────────────────────────────── */
    printf("\n[malloc]\n");

    char *heap = (char *)malloc(64);
    if (heap) {
        strcpy(heap, "heap allocation works!");
        printf("  malloc(64): \"%s\"\n", heap);
        free(heap);
        printf("  free(): ok (bump — no-op)\n");
    } else {
        printf("  malloc FAILED\n");
    }

    /* ── 6. printf format specifiers ────────────────────────────── */
    printf("\n[printf]\n");
    printf("  %%d  : %d\n",  -12345);
    printf("  %%u  : %u\n",  99999u);
    printf("  %%x  : %x\n",  0xDEAD);
    printf("  %%X  : %X\n",  0xBEEF);
    printf("  %%llu: %llu\n", (unsigned long long)18446744073709551615ULL);
    printf("  %%lld: %lld\n", (long long)-9223372036854775807LL);
    printf("  %%c  : %c\n",  'K');
    printf("  %%%%  : %%\n");

    /* ── 7. Uptime + meminfo ─────────────────────────────────────── */
    printf("\n[ironkernel.h]\n");

    uint64_t ticks = ik_uptime();
    uint64_t secs  = ticks / 100;
    uint64_t mins  = secs  / 60; secs %= 60;
    printf("  uptime : %llum %llus (%llu ticks)\n", mins, secs, ticks);

    ik_meminfo_t mi;
    ik_meminfo(&mi);
    printf("  memory : %llu KB free / %llu KB total\n",
           mi.free_kb, mi.total_kb);

    char cwd[64];
    ik_getcwd(cwd, sizeof(cwd));
    printf("  cwd    : %s\n", cwd);

    /* ── 8. File I/O ─────────────────────────────────────────────── */
    printf("\n[file I/O]\n");

    ik_mkdir("DEMO");
    ik_chdir("DEMO");

    const char *msg = "written by iklibc demo program\n";
    int r = ik_write_file("NOTES.TXT", msg, strlen(msg));
    printf("  write NOTES.TXT : %s\n", r == 0 ? "ok" : "FAILED");

    char rbuf[128];
    uint64_t rb = ik_read_file("NOTES.TXT", rbuf, sizeof(rbuf) - 1);
    if (rb != (uint64_t)-1) {
        rbuf[rb] = '\0';
        printf("  read back (%llu bytes): %s", rb, rbuf);
    } else {
        printf("  read FAILED\n");
    }

    ik_chdir("..");

    /* ── 9. Interactive input ────────────────────────────────────── */
    printf("\n[input]\n");
    printf("  Enter your name: ");
    char name[32];
    gets_s(name, sizeof(name));
    printf("  Hello, %s!\n", name);

    /* ── 10. bool ────────────────────────────────────────────────── */
    bool flag = (strlen(name) > 0);
    printf("\n[stdbool]  name non-empty: %s\n", flag ? "true" : "false");

    /* ── Done ────────────────────────────────────────────────────── */
    printf("\n================================\n");
    printf("  iklibc demo complete\n");
    printf("  Files on disk: DEMO/NOTES.TXT\n");
    printf("================================\n\n");

    return 0;
}
