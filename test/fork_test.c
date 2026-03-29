/* IronKernel fork test — test/fork_test.c
   Demonstrates SYS_FORK + SYS_WAITPID:
     - parent forks a child
     - child does some work and exits with a status
     - parent collects exit status via waitpid */

#include <stdio.h>
#include <stdlib.h>
#include <ironkernel.h>

int main(void)
{
    printf("=== fork_test ===\n");
    printf("parent pid: %d\n", 0);  /* task 0 = shell, we are task 1 */

    int pid = ik_fork();

    if (pid < 0) {
        printf("fork failed!\n");
        return 1;
    }

    if (pid == 0) {
        /* ── Child ─────────────────────────────────── */
        printf("[child]  I am the child, counting to 3...\n");
        for (int i = 1; i <= 3; i++) {
            printf("[child]  %d\n", i);
        }
        printf("[child]  exiting with status 42\n");
        exit(42);
    }

    /* ── Parent ─────────────────────────────────── */
    printf("[parent] forked child pid = %d\n", pid);
    printf("[parent] waiting for child...\n");

    int status = 0;
    int ret = ik_waitpid(pid, &status);

    if (ret < 0) {
        printf("[parent] waitpid failed!\n");
        return 1;
    }

    printf("[parent] child exited with status %d\n", status);
    printf("=== done ===\n");
    return 0;
}
