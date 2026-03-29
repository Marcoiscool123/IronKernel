/* IronKernel v0.04 — test/hello.c  (ELF64 user-mode test)
   Syscall convention: RAX = number, RBX = arg, int 0x80 */

static void sys_write(const char *s)
{
    __asm__ volatile (
        "mov $0, %%rax\n\t"   /* SYS_WRITE = 0 */
        "mov %0, %%rbx\n\t"   /* RBX = string pointer */
        "int $0x80"
        :: "r"((unsigned long long)s)
        : "rax", "rbx"
    );
}

static void sys_exit(void)
{
    __asm__ volatile (
        "mov $1, %%rax\n\t"   /* SYS_EXIT = 1 */
        "int $0x80"
        ::: "rax"
    );
}

void _start(void)
{
    sys_write("BOI WASSUP GNG DUDE BOI\n");
    sys_exit();
    /* __builtin_unreachable prevents a spurious ret into garbage */
    __builtin_unreachable();
}
