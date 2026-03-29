/* ERROR.ELF — IronKernel kernel panic demonstration.
   Calls SYS_TEST_PANIC (32) which executes in ring-0 syscall context
   and calls panic() directly — produces a TRUE kernel panic screen
   with ring-0 RIP, RSP, and a walkable kernel stack trace.

   Works in both text mode and GUI mode (no ik_gfx_init). */

#include <ironkernel.h>
#include <ikgfx.h>   /* for ik_read_key / IK_KEY_ENTER only */

int main(void)
{
    ik_write("\n");
    ik_write("  +------------------------------------------+\n");
    ik_write("  |        ERROR.ELF  --  Panic Demo         |\n");
    ik_write("  +------------------------------------------+\n");
    ik_write("\n");
    ik_write("  Calls SYS_TEST_PANIC (syscall 32).\n");
    ik_write("  The kernel handles it in ring-0 and calls\n");
    ik_write("  panic() directly -- full kernel panic screen\n");
    ik_write("  with ring-0 RIP and kernel stack trace.\n");
    ik_write("\n");
    ik_write("  Press ENTER to trigger panic...\n");

    int k;
    do { k = ik_read_key(); } while (k != IK_KEY_ENTER);

    /* SYS_TEST_PANIC = 32 — runs in ring-0, calls panic() */
    __asm__ volatile(
        "mov $32, %%rax\n\t"
        "int $0x80"
        ::: "rax"
    );

    /* unreachable */
    return 0;
}
