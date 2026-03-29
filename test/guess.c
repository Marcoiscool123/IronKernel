// No includes. Pure syscall.

typedef unsigned long size_t;

static long sys_write(int fd, const void *buf, size_t count)
{
    long ret;
    asm volatile (
        "mov $1, %%rax\n"
        "syscall"
        : "=a"(ret)
        : "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static long sys_read(int fd, void *buf, size_t count)
{
    long ret;
    asm volatile (
        "mov $0, %%rax\n"
        "syscall"
        : "=a"(ret)
        : "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void sys_exit(int code)
{
    asm volatile (
        "mov $60, %%rax\n"
        "syscall"
        :
        : "D"(code)
        : "rcx", "r11", "memory"
    );
}

int str_to_int(char *buf)
{
    int n = 0;
    for(int i=0; buf[i] >= '0' && buf[i] <= '9'; i++)
        n = n*10 + (buf[i]-'0');
    return n;
}

void _start()
{
    char buf[16];
    int secret = 42;

    while(1)
    {
        sys_write(1, "Guess number (0-100): ", 23);
        sys_read(0, buf, 16);

        int guess = str_to_int(buf);

        if(guess == secret)
        {
            sys_write(1, "Correct!\n", 9);
            break;
        }
        else if(guess < secret)
        {
            sys_write(1, "Too low\n", 8);
        }
        else
        {
            sys_write(1, "Too high\n", 9);
        }
    }

    sys_exit(0);
}
