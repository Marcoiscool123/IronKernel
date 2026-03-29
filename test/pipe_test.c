/* IronKernel pipe test — test/pipe_test.c
   Demonstrates SYS_PIPE + SYS_DUP2 + SYS_CLOSE:
     - parent creates a pipe
     - forks a child
     - child redirects stdout (fd 1) to the pipe write end
     - child writes two lines via printf (which uses SYS_WRITE → fd 1)
     - parent reads everything from the pipe read end and prints it */

#include <stdio.h>
#include <stdlib.h>
#include <ironkernel.h>

int main(void)
{
    printf("=== pipe_test ===\n");

    int pipefd[2];
    if (ik_pipe(pipefd) < 0) {
        printf("pipe() failed\n");
        return 1;
    }
    printf("pipe: read_fd=%d  write_fd=%d\n", pipefd[0], pipefd[1]);

    int pid = ik_fork();
    if (pid < 0) {
        printf("fork() failed\n");
        return 1;
    }

    if (pid == 0) {
        /* ── Child ────────────────────────────────────────────────── */
        /* Point stdout at the pipe write end, then close the raw fds. */
        ik_dup2(pipefd[1], 1);
        ik_close(pipefd[0]);
        ik_close(pipefd[1]);

        /* These printfs now go into the pipe instead of the screen. */
        printf("hello from child via pipe!\n");
        printf("second line from child\n");
        exit(0);
    }

    /* ── Parent ───────────────────────────────────────────────────── */
    /* Close the write end so SYS_READ_FD gets EOF when child exits. */
    ik_close(pipefd[1]);

    char buf[512];
    int n = ik_read_fd(pipefd[0], buf, (int)sizeof(buf) - 1);
    buf[n] = '\0';
    ik_close(pipefd[0]);

    printf("[parent] read %d bytes from pipe:\n", n);
    printf("%s", buf);

    int status = 0;
    ik_waitpid(pid, &status);
    printf("=== done (child exit=%d) ===\n", status);
    return 0;
}
