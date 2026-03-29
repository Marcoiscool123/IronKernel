/* IronKernel iklibc — syscall.c
   All 11 int $0x80 syscall implementations.
   Convention: RAX=number  RBX=arg1  RCX=arg2  RDX=arg3 */
#include <ironkernel.h>
#include <errno.h>

void ik_write(const char *s)
{
    __asm__ volatile(
        "mov $0, %%rax\n\t"
        "mov %0, %%rbx\n\t"
        "int $0x80"
        :: "r"((uint64_t)s) : "rax", "rbx", "memory");
}

__attribute__((noreturn))
void ik_exit(int status)
{
    __asm__ volatile(
        "mov $1, %%rax\n\t"
        "mov %0, %%rbx\n\t"
        "int $0x80"
        :: "r"((uint64_t)(int64_t)status) : "rax", "rbx");
    __builtin_unreachable();
}

int ik_fork(void)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $11, %%rax\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        :: "rax");
    if (ret == (uint64_t)-1) { errno = ENOMEM; return -1; }
    return (int)ret;
}

int ik_waitpid(int pid, int *status)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $12, %%rax\n\t"
        "mov %1,  %%rbx\n\t"
        "mov %2,  %%rcx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)(int64_t)pid), "r"((uint64_t)status)
        : "rax", "rbx", "rcx");
    if (ret == (uint64_t)-1) { errno = EINVAL; return -1; }
    return (int)ret;
}

uint64_t ik_read(char *buf, uint64_t bufsz)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $2, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "mov %2, %%rcx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)buf), "r"(bufsz)
        : "rax", "rbx", "rcx", "memory");
    return ret;
}

int ik_write_file(const char *path, const void *data, uint64_t len)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $3, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "mov %2, %%rcx\n\t"
        "mov %3, %%rdx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)path), "r"((uint64_t)data), "r"(len)
        : "rax", "rbx", "rcx", "rdx", "memory");
    if (ret == (uint64_t)-1) { errno = EIO; return -1; }
    return 0;
}

int ik_mkdir(const char *name)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $4, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)name)
        : "rax", "rbx");
    if (ret == (uint64_t)-1) { errno = EEXIST; return -1; }
    return 0;
}

uint64_t ik_getcwd(char *buf, uint64_t bufsz)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $5, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "mov %2, %%rcx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)buf), "r"(bufsz)
        : "rax", "rbx", "rcx", "memory");
    return ret;
}

int ik_chdir(const char *name)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $6, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)name)
        : "rax", "rbx");
    if (ret == (uint64_t)-1) { errno = ENOENT; return -1; }
    return 0;
}

uint64_t ik_read_file(const char *path, void *buf, uint64_t bufsz)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $7, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "mov %2, %%rcx\n\t"
        "mov %3, %%rdx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)path), "r"((uint64_t)buf), "r"(bufsz)
        : "rax", "rbx", "rcx", "rdx", "memory");
    if (ret == (uint64_t)-1) { errno = ENOENT; }
    return ret;
}

int ik_delete(const char *path)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $8, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)path)
        : "rax", "rbx");
    if (ret == (uint64_t)-1) { errno = ENOENT; return -1; }
    return 0;
}

uint64_t ik_uptime(void)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $9, %%rax\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        :: "rax");
    return ret;
}

void ik_meminfo(ik_meminfo_t *out)
{
    __asm__ volatile(
        "mov $10, %%rax\n\t"
        "mov %0,  %%rbx\n\t"
        "int $0x80"
        :: "r"((uint64_t)out) : "rax", "rbx", "memory");
}

int ik_pipe(int pipefd[2])
{
    uint64_t ret;
    __asm__ volatile(
        "mov $13, %%rax\n\t"
        "mov %1,  %%rbx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)pipefd)
        : "rax", "rbx", "memory");
    return (ret == (uint64_t)-1) ? -1 : 0;
}

int ik_dup2(int oldfd, int newfd)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $14, %%rax\n\t"
        "mov %1,  %%rbx\n\t"
        "mov %2,  %%rcx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)(int64_t)oldfd), "r"((uint64_t)(int64_t)newfd)
        : "rax", "rbx", "rcx");
    return (ret == (uint64_t)-1) ? -1 : (int)ret;
}

int ik_close(int fd)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $15, %%rax\n\t"
        "mov %1,  %%rbx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)(int64_t)fd)
        : "rax", "rbx");
    return (ret == (uint64_t)-1) ? -1 : 0;
}

int ik_read_fd(int fd, void *buf, int len)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $16, %%rax\n\t"
        "mov %1,  %%rbx\n\t"
        "mov %2,  %%rcx\n\t"
        "mov %3,  %%rdx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)(int64_t)fd),
          "r"((uint64_t)buf),
          "r"((uint64_t)(int64_t)len)
        : "rax", "rbx", "rcx", "rdx", "memory");
    return (ret == (uint64_t)-1) ? -1 : (int)ret;
}

int ik_write_fd(int fd, const void *buf, int len)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $17, %%rax\n\t"
        "mov %1,  %%rbx\n\t"
        "mov %2,  %%rcx\n\t"
        "mov %3,  %%rdx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)(int64_t)fd),
          "r"((uint64_t)buf),
          "r"((uint64_t)(int64_t)len)
        : "rax", "rbx", "rcx", "rdx", "memory");
    return (ret == (uint64_t)-1) ? -1 : (int)ret;
}

int ik_exec(const char *path)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $18, %%rax\n\t"
        "mov %1,  %%rbx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)path)
        : "rax", "rbx", "memory");
    /* Only reached on failure */
    return -1;
    (void)ret;
}

int ik_readdir(ik_dirent_t *buf, int max)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $19, %%rax\n\t"
        "mov %1,  %%rbx\n\t"
        "mov %2,  %%rcx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)buf),
          "r"((uint64_t)(int64_t)max)
        : "rax", "rbx", "rcx", "memory");
    return (int)ret;
}

uint64_t ik_get_arg(char *buf, uint64_t bufsz)
{
    uint64_t ret;
    __asm__ volatile(
        "mov $31, %%rax\n\t"
        "mov %1,  %%rbx\n\t"
        "mov %2,  %%rcx\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((uint64_t)buf), "r"(bufsz)
        : "rax", "rbx", "rcx", "memory");
    return ret;
}

void ik_beep(uint32_t freq, uint32_t ms)
{
    __asm__ volatile(
        "mov $33,  %%rax\n\t"
        "mov %0,   %%rbx\n\t"
        "mov %1,   %%rcx\n\t"
        "int $0x80"
        :: "r"((uint64_t)freq), "r"((uint64_t)ms)
        : "rax", "rbx", "rcx");
}
