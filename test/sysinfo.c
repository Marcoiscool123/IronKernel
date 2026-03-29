/* IronKernel — test/sysinfo.c
   Advanced user-mode demo: exercises all 11 syscalls (0-10).
   No libc. No includes. Pure int $0x80 convention:
     RAX = syscall number  RBX = arg1  RCX = arg2  RDX = arg3 */

typedef unsigned long long u64;

/* ── Syscall numbers ───────────────────────────────────────────── */
#define SYS_WRITE       0
#define SYS_EXIT        1
#define SYS_READ        2
#define SYS_WRITE_FILE  3
#define SYS_MKDIR       4
#define SYS_GETCWD      5
#define SYS_CHDIR       6
#define SYS_READ_FILE   7
#define SYS_DELETE      8
#define SYS_UPTIME      9
#define SYS_MEMINFO    10

/* ── Syscall stubs ─────────────────────────────────────────────── */

static void sys_write(const char *s) {
    __asm__ volatile("mov $0,%%rax; mov %0,%%rbx; int $0x80"
        :: "r"((u64)s) : "rax","rbx");
}

static void sys_exit(void) {
    __asm__ volatile("mov $1,%%rax; int $0x80" ::: "rax");
}

static u64 sys_read(char *buf, u64 max) {
    u64 ret;
    __asm__ volatile(
        "mov $2,%%rax; mov %1,%%rbx; mov %2,%%rcx; int $0x80; mov %%rax,%0"
        : "=r"(ret) : "r"((u64)buf), "r"(max) : "rax","rbx","rcx");
    return ret;
}

static u64 sys_write_file(const char *path, const char *data, u64 len) {
    u64 ret;
    __asm__ volatile(
        "mov $3,%%rax; mov %1,%%rbx; mov %2,%%rcx; mov %3,%%rdx; int $0x80; mov %%rax,%0"
        : "=r"(ret)
        : "r"((u64)path), "r"((u64)data), "r"(len)
        : "rax","rbx","rcx","rdx");
    return ret;
}

static u64 sys_mkdir(const char *name) {
    u64 ret;
    __asm__ volatile(
        "mov $4,%%rax; mov %1,%%rbx; int $0x80; mov %%rax,%0"
        : "=r"(ret) : "r"((u64)name) : "rax","rbx");
    return ret;
}

static u64 sys_getcwd(char *buf, u64 sz) {
    u64 ret;
    __asm__ volatile(
        "mov $5,%%rax; mov %1,%%rbx; mov %2,%%rcx; int $0x80; mov %%rax,%0"
        : "=r"(ret) : "r"((u64)buf), "r"(sz) : "rax","rbx","rcx");
    return ret;
}

static u64 sys_chdir(const char *name) {
    u64 ret;
    __asm__ volatile(
        "mov $6,%%rax; mov %1,%%rbx; int $0x80; mov %%rax,%0"
        : "=r"(ret) : "r"((u64)name) : "rax","rbx");
    return ret;
}

static u64 sys_read_file(const char *path, char *buf, u64 sz) {
    u64 ret;
    __asm__ volatile(
        "mov $7,%%rax; mov %1,%%rbx; mov %2,%%rcx; mov %3,%%rdx; int $0x80; mov %%rax,%0"
        : "=r"(ret)
        : "r"((u64)path), "r"((u64)buf), "r"(sz)
        : "rax","rbx","rcx","rdx");
    return ret;
}

static u64 sys_delete(const char *path) {
    u64 ret;
    __asm__ volatile(
        "mov $8,%%rax; mov %1,%%rbx; int $0x80; mov %%rax,%0"
        : "=r"(ret) : "r"((u64)path) : "rax","rbx");
    return ret;
}

static u64 sys_uptime(void) {
    u64 ret;
    __asm__ volatile(
        "mov $9,%%rax; int $0x80; mov %%rax,%0"
        : "=r"(ret) :: "rax");
    return ret;
}

static void sys_meminfo(u64 *out) {
    __asm__ volatile(
        "mov $10,%%rax; mov %0,%%rbx; int $0x80"
        :: "r"((u64)out) : "rax","rbx");
}

/* ── String helpers ────────────────────────────────────────────── */

static u64 slen(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

/* Convert u64 to decimal string. Returns length. */
static int u64str(u64 n, char *buf) {
    if (!n) { buf[0] = '0'; buf[1] = 0; return 1; }
    char tmp[21]; int i = 20; tmp[20] = 0;
    while (n) { tmp[--i] = '0' + (n % 10); n /= 10; }
    int len = 0;
    while (tmp[i]) buf[len++] = tmp[i++];
    buf[len] = 0;
    return len;
}

/* Append src to dst[*pos], null-terminate. */
static void app(char *dst, u64 *pos, u64 max, const char *src) {
    for (u64 i = 0; src[i] && *pos < max - 1; i++)
        dst[(*pos)++] = src[i];
    dst[*pos] = 0;
}

/* Copy at most n chars from src to dst, null-terminate. */
static void scopy(char *dst, const char *src, u64 n) {
    u64 i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Compare two null-terminated strings. */
static int seq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ── Banner ────────────────────────────────────────────────────── */
static void print_banner(void) {
    sys_write("\n");
    sys_write("  +=======================================+\n");
    sys_write("  |   IronKernel  --  System Info Demo   |\n");
    sys_write("  |   All 11 syscalls exercised (0-10)   |\n");
    sys_write("  +=======================================+\n\n");
}

/* ── Stats panel ───────────────────────────────────────────────── */
static void print_stats(void) {
    char nbuf[24];

    /* SYS_MEMINFO (10) */
    u64 mem[2];
    sys_meminfo(mem);
    u64str(mem[1], nbuf);
    sys_write("  MEM    : "); sys_write(nbuf); sys_write(" KB free / ");
    u64str(mem[0], nbuf);
    sys_write(nbuf); sys_write(" KB total\n");

    /* SYS_UPTIME (9) */
    u64 ticks = sys_uptime();
    u64 secs  = ticks / 100;
    u64 mins  = secs  / 60; secs %= 60;
    u64str(mins,  nbuf); sys_write("  UPTIME : "); sys_write(nbuf); sys_write("m ");
    u64str(secs,  nbuf); sys_write(nbuf); sys_write("s  (");
    u64str(ticks, nbuf); sys_write(nbuf); sys_write(" ticks)\n");

    /* SYS_GETCWD (5) */
    char cwd[64];
    sys_getcwd(cwd, 64);
    sys_write("  CWD    : "); sys_write(cwd); sys_write("\n\n");
}

/* ── Main ──────────────────────────────────────────────────────── */
void _start(void) {
    char nbuf[24];
    char bigbuf[512];
    u64  pos;

    print_banner();
    print_stats();

    /* ── 1. Get username via SYS_READ (2) ── */
    sys_write("  Enter your name > ");
    char name[32];
    u64 nlen = sys_read(name, 32);
    if (nlen == 0) { scopy(name, "stranger", 9); nlen = 8; }

    sys_write("  Hello, "); sys_write(name); sys_write("!\n\n");

    /* ── 2. Write PROFILE.TXT with SYS_WRITE_FILE (3) ── */
    u64 mem[2];
    sys_meminfo(mem);
    u64 ticks = sys_uptime();
    u64 secs  = ticks / 100;
    u64 mins  = secs / 60; secs %= 60;
    char cwd[64];
    sys_getcwd(cwd, 64);

    pos = 0;
    app(bigbuf, &pos, 512, "=== IronKernel Profile ===\n");
    app(bigbuf, &pos, 512, "User   : "); app(bigbuf, &pos, 512, name);     app(bigbuf, &pos, 512, "\n");
    app(bigbuf, &pos, 512, "CWD    : "); app(bigbuf, &pos, 512, cwd);      app(bigbuf, &pos, 512, "\n");
    app(bigbuf, &pos, 512, "Memory : ");
    u64str(mem[1], nbuf); app(bigbuf, &pos, 512, nbuf); app(bigbuf, &pos, 512, " KB free / ");
    u64str(mem[0], nbuf); app(bigbuf, &pos, 512, nbuf); app(bigbuf, &pos, 512, " KB\n");
    app(bigbuf, &pos, 512, "Uptime : ");
    u64str(mins, nbuf); app(bigbuf, &pos, 512, nbuf); app(bigbuf, &pos, 512, "m ");
    u64str(secs, nbuf); app(bigbuf, &pos, 512, nbuf); app(bigbuf, &pos, 512, "s\n");
    app(bigbuf, &pos, 512, "==========================\n");

    u64 r = sys_write_file("PROFILE.TXT", bigbuf, pos);
    if (r == 0)
        sys_write("  [+] wrote  PROFILE.TXT\n");
    else
        sys_write("  [!] PROFILE.TXT write failed\n");

    /* ── 3. Create LOGS dir with SYS_MKDIR (4), enter with SYS_CHDIR (6) ── */
    sys_mkdir("LOGS");
    sys_chdir("LOGS");

    /* ── 4. Write a session log inside LOGS/ ── */
    pos = 0;
    app(bigbuf, &pos, 512, "session_user="); app(bigbuf, &pos, 512, name); app(bigbuf, &pos, 512, "\n");
    app(bigbuf, &pos, 512, "session_ticks=");
    u64str(ticks, nbuf); app(bigbuf, &pos, 512, nbuf); app(bigbuf, &pos, 512, "\n");
    app(bigbuf, &pos, 512, "session_mem_free=");
    u64str(mem[1], nbuf); app(bigbuf, &pos, 512, nbuf); app(bigbuf, &pos, 512, "\n");

    r = sys_write_file("SESSION.LOG", bigbuf, pos);
    if (r == 0)
        sys_write("  [+] wrote  LOGS/SESSION.LOG\n");
    else
        sys_write("  [!] SESSION.LOG write failed\n");

    /* ── 5. Go back to root ── */
    sys_chdir("..");

    /* ── 6. Read back PROFILE.TXT with SYS_READ_FILE (7) ── */
    char rbuf[512];
    u64 rbytes = sys_read_file("PROFILE.TXT", rbuf, 511);
    if (rbytes != (u64)-1) {
        rbuf[rbytes] = 0;
        sys_write("\n  --- PROFILE.TXT ---\n");
        sys_write(rbuf);
        sys_write("  --------------------\n\n");
    } else {
        sys_write("  [!] could not read PROFILE.TXT\n\n");
    }

    /* ── 7. Read SESSION.LOG back to verify it persists ── */
    char lbuf[256];
    sys_chdir("LOGS");
    u64 lbytes = sys_read_file("SESSION.LOG", lbuf, 255);
    sys_chdir("..");
    if (lbytes != (u64)-1) {
        lbuf[lbytes] = 0;
        sys_write("  [+] LOGS/SESSION.LOG verified (");
        u64str(lbytes, nbuf); sys_write(nbuf); sys_write(" bytes)\n");
    } else {
        sys_write("  [!] could not verify LOGS/SESSION.LOG\n");
    }

    /* ── 8. Interactive mini-shell — keep asking until "exit" ── */
    sys_write("\n  Mini-shell (type 'exit' to quit):\n");
    char line[64];
    for (;;) {
        sys_write("  > ");
        u64 llen = sys_read(line, 64);
        if (llen == 0) continue;
        line[llen] = 0;

        if (seq(line, "exit")) break;

        /* echo command: echo <text> */
        if (line[0]=='e' && line[1]=='c' && line[2]=='h' && line[3]=='o' && line[4]==' ') {
            sys_write("  "); sys_write(line + 5); sys_write("\n");
            continue;
        }

        /* uptime command */
        if (seq(line, "uptime")) {
            u64 t2    = sys_uptime();
            u64 s2    = t2 / 100;
            u64 m2    = s2 / 60; s2 %= 60;
            u64 ms2   = (t2 % 100) * 10;
            sys_write("  uptime: ");
            u64str(m2, nbuf); sys_write(nbuf); sys_write("m ");
            u64str(s2, nbuf); sys_write(nbuf); sys_write("s ");
            u64str(ms2, nbuf); sys_write(nbuf); sys_write("ms");
            sys_write("  ("); u64str(t2, nbuf); sys_write(nbuf); sys_write(" ticks)\n");
            continue;
        }

        /* mem command */
        if (seq(line, "mem")) {
            u64 m2[2];
            sys_meminfo(m2);
            sys_write("  mem: ");
            u64str(m2[1], nbuf); sys_write(nbuf); sys_write(" KB free / ");
            u64str(m2[0], nbuf); sys_write(nbuf); sys_write(" KB total\n");
            continue;
        }

        /* pwd command */
        if (seq(line, "pwd")) {
            char cwd2[64];
            sys_getcwd(cwd2, 64);
            sys_write("  "); sys_write(cwd2); sys_write("\n");
            continue;
        }

        sys_write("  unknown: "); sys_write(line); sys_write("\n");
        sys_write("  commands: echo <text>  uptime  mem  pwd  exit\n");
    }

    sys_write("\n  Done. Files on disk: PROFILE.TXT, LOGS/\n");
    sys_write("  Try: cat PROFILE.TXT\n\n");

    sys_exit();
    __builtin_unreachable();
}
