/* IronKernel — test/wsp.c
   Creates a file WSP.TXT containing "WSP" on the FAT12 disk.
   Syscall convention: RAX = number, RBX = arg1, RCX = arg2, RDX = arg3
   int $0x80 */

static void sys_write(const char *s)
{
    __asm__ volatile (
        "mov $0, %%rax\n\t"
        "mov %0, %%rbx\n\t"
        "int $0x80"
        :: "r"((unsigned long long)s)
        : "rax", "rbx"
    );
}

static void sys_write_file(const char *path, const char *data, unsigned long long len)
{
    __asm__ volatile (
        "mov $3, %%rax\n\t"   /* SYS_WRITE_FILE = 3 */
        "mov %0, %%rbx\n\t"   /* RBX = filename ptr  */
        "mov %1, %%rcx\n\t"   /* RCX = data ptr      */
        "mov %2, %%rdx\n\t"   /* RDX = byte count    */
        "int $0x80"
        :: "r"((unsigned long long)path),
           "r"((unsigned long long)data),
           "r"(len)
        : "rax", "rbx", "rcx", "rdx"
    );
}

static void sys_exit(void)
{
    __asm__ volatile (
        "mov $1, %%rax\n\t"
        "int $0x80"
        ::: "rax"
    );
}

void _start(void)
{
    sys_write_file("WSP.TXT", "WSP", 3);
    sys_write("created WSP.TXT\n");
    sys_exit();
    __builtin_unreachable();
}
