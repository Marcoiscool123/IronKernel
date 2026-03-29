/* ikls.c — standalone ls ELF for IronKernel iksh */
#include <stdio.h>
#include <string.h>
#include <ironkernel.h>

static ik_dirent_t dir[128];

static void print_size(uint32_t sz)
{
    if      (sz >= 1024*1024) printf("%u MB", sz/(1024*1024));
    else if (sz >= 1024)      printf("%u KB", sz/1024);
    else                      printf("%u B",  sz);
}

int main(void)
{
    int n = ik_readdir(dir, 128);
    if (n == 0) { printf("(empty)\n"); return 0; }

    for (int i = 0; i < n; i++) {
        if (dir[i].is_dir) {
            printf("  [%s]\n", dir[i].name);
        } else {
            char full[14];
            strcpy(full, dir[i].name);
            if (dir[i].ext[0]) { strcat(full, "."); strcat(full, dir[i].ext); }
            printf("  %-13s  ", full);
            print_size(dir[i].size);
            printf("\n");
        }
    }
    return 0;
}
