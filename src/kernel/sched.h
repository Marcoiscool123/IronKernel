/* IronKernel — sched.h */
#ifndef SCHED_H
#define SCHED_H
#include "types.h"
#include "idt.h"     /* InterruptFrame — stored in task_t for fork */

#define MAX_TASKS        32
#define TASK_STACK_SIZE  65536  /* 64 KB kernel stack per task */
#define TASK_IRQ_STACK_SIZE 4096 /* 4 KB dedicated ring-3 interrupt stack */
#define TASK_MAX_FDS     16     /* file descriptors per task  */

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_DEAD    = 2,
    TASK_ZOMBIE  = 3,   /* fork child exited; waiting for waitpid cleanup */
} task_state_t;

/* Per-task file descriptor entry. */
typedef enum {
    FD_NONE       = 0,
    FD_STDIN,        /* keyboard                  */
    FD_STDOUT,       /* VGA console               */
    FD_PIPE_READ,    /* read end of a kernel pipe */
    FD_PIPE_WRITE,   /* write end of a kernel pipe */
} fd_type_t;

typedef struct {
    fd_type_t type;
    int       pipe_id;   /* index into g_pipes[] — valid for PIPE_READ/WRITE */
} fd_entry_t;

typedef struct {
    uint64_t      rsp;                  /* saved kernel RSP              */
    uint8_t      *stack;                /* heap-allocated stack base     */
    task_state_t  state;
    uint32_t      id;
    char          name[16];
    uint32_t      ticks;
    uint64_t      cr3;                  /* per-process page table (CR3)  */
    uint64_t      kstack_top;           /* kernel stack top               */
    uint8_t      *irq_stack;            /* dedicated ring-3 interrupt stack */
    uint64_t      irq_stack_top;        /* TSS.rsp0 set to this on ring-3 entry */

    /* Fork support */
    int           exit_code;            /* set by SYS_EXIT               */
    int           parent_id;            /* task id of forking parent     */
    int           is_forked;            /* 1 = fork child (not exec'd)   */
    InterruptFrame fork_frame;          /* saved user state at fork()    */

    /* File descriptor table */
    fd_entry_t    fds[TASK_MAX_FDS];

    /* WM window binding (set by wm_spawn_elf / SYS_WIN_CREATE) */
    int  win_id;        /* >= 0 → stdout routes to this WM window; -1 = VGA */
    char elf_name[9];   /* FAT 8.3 name (space-padded, null-terminated) */
    char elf_ext[4];    /* FAT 8.3 ext (space-padded, null-terminated)  */
    char elf_arg[32];   /* first argument passed after ELF name on exec  */
} task_t;

void    sched_init(void);
int     task_spawn(const char *name, void (*func)(void));
void    task_exit(void);
int     sched_fork(InterruptFrame *frame);  /* clone current task (SYS_FORK) */
void    sched_tick(void);
void    sched_yield(void);

/* Defined in boot/switch.asm — swaps kernel stack pointers */
void task_switch(uint64_t *old_rsp, uint64_t new_rsp);

task_t *sched_get_tasks(void);
int     sched_get_count(void);
int     sched_current_id(void);

extern volatile int user_mode_active;
void sched_lock(void);
void sched_unlock(void);
#endif
