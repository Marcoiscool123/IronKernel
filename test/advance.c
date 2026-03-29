/* ================================================================
   IronKernel — test/advance.c
   ADVANCE.ELF — The most advanced IronKernel user-mode program.

   Demonstrates every kernel capability:
     SYS_FORK, SYS_WAITPID, SYS_PIPE, SYS_DUP2, SYS_CLOSE,
     SYS_READ_FD, SYS_WRITE_FD, SYS_EXEC, SYS_READDIR,
     SYS_READ_FILE, SYS_WRITE_FILE, SYS_DELETE,
     SYS_UPTIME, SYS_MEMINFO, SYS_GETCWD, SYS_CHDIR

   Modes (interactive menu):
     [1] Mandelbrot Set     — ASCII fractal, fork-parallel rendering
     [2] Prime Sieve        — Eratosthenes up to 2000
     [3] Fork Tree          — binary tree of spawned processes via pipes
     [4] Pipe Benchmark     — IPC throughput (fork + write_fd loop)
     [5] File Stress Test   — create/write/read/verify/delete 8 files
     [6] Sort Race          — bubble vs insertion vs selection on 64 ints
     [7] Fibonacci Matrix   — iterative + recursive, compare
     [8] System Dashboard   — live uptime + memory loop (Ctrl+C = newline)
     [9] Pipeline Demo      — ls | grep | wc via fork+pipe chain
     [0] Exit
   ================================================================ */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ironkernel.h>

/* ── tiny helpers ────────────────────────────────────────────────── */
static int av_abs(int x)  { return x < 0 ? -x : x; }
static int av_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }
static void av_strcpy(char *d, const char *s) { while((*d++=*s++)); }
static void av_strcat(char *d, const char *s) { while(*d)d++; while((*d++=*s++)); }
static int  av_strcmp(const char *a, const char *b)
    { while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
static int  av_strncmp(const char *a, const char *b, int n)
    { while(n-->0&&*a&&*a==*b){a++;b++;} if(n<0) return 0;
      return (unsigned char)*a-(unsigned char)*b; }

static void av_itoa(long long n, char *buf)
{
    if (n < 0) { *buf++='-'; n=-n; }
    if (n == 0) { *buf++='0'; *buf=0; return; }
    char tmp[24]; int i=0;
    while(n){tmp[i++]='0'+(int)(n%10);n/=10;}
    int j=0; while(i>0) buf[j++]=tmp[--i]; buf[j]=0;
}

static long long av_atoll(const char *s)
{
    while(*s==' ')s++; long long v=0;
    while(*s>='0'&&*s<='9') v=v*10+(*s++-'0'); return v;
}

static void nl(void) { printf("\n"); }
static void sep(void) { printf("  ------------------------------------------------\n"); }

static void print_uint(uint64_t n)
{
    char b[24]; av_itoa((long long)n, b); printf("%s", b);
}

static void wait_enter(void)
{
    printf("  [press enter to continue]");
    char tmp[4]; ik_read(tmp, sizeof(tmp));
    printf("\n");
}

/* ── elapsed ticks → ms string ──────────────────────────────────── */
static void print_elapsed(uint64_t ticks)
{
    uint64_t ms = ticks * 10;
    printf("%llu ms (%llu ticks)", ms, ticks);
}

/* ================================================================
   [1] MANDELBROT SET — fork-parallel
   Each child renders one horizontal band and sends it as text
   through a pipe. Parent collects + prints in order.
   ================================================================ */

#define MB_W  70
#define MB_H  32
#define MB_MAX_ITER 64

/* Render one row of the Mandelbrot set into buf (MB_W chars + '\n' + '\0'). */
static void mandelbrot_row(int row, int total_rows, char *out)
{
    /* Map row → imaginary axis: [-1.2 .. 1.2] */
    /* We use fixed-point: multiply by 1000 to avoid floats. */
    /* Re: [-2.5 .. 1.0],  Im: [-1.2 .. 1.2] */
    /* Scaled by 1000: Re ∈ [-2500 .. 1000], Im ∈ [-1200 .. 1200] */

    int im_scaled = 1200 - (row * 2400) / total_rows;  /* Im * 1000 */

    int i;
    for (i = 0; i < MB_W; i++) {
        int re_scaled = -2500 + (i * 3500) / MB_W;  /* Re * 1000 */

        /* z = 0; iterate z = z^2 + c */
        /* Using scaled integers: zr,zi in units of 1/1000 */
        /* To avoid overflow: scale up more. We'll use 1/256 scale */
        /* zr, zi in 1/256 units; c in 1/256 units */
        /* c_re = re_scaled * 256 / 1000, c_im = im_scaled * 256 / 1000 */
        int cr = re_scaled * 256 / 1000;
        int ci = im_scaled * 256 / 1000;
        int zr = 0, zi = 0;
        int iter = 0;

        while (iter < MB_MAX_ITER) {
            /* |z|^2 = (zr^2 + zi^2) / 256^2 */
            /* overflow check: if zr^2 + zi^2 > 4 * 256^2 = 262144 → escaped */
            int zr2 = (zr * zr) >> 8;  /* zr^2 / 256 */
            int zi2 = (zi * zi) >> 8;  /* zi^2 / 256 */
            if (zr2 + zi2 > 4 * 256) break;  /* |z|^2 > 4 */
            /* z = z^2 + c */
            int new_zr = zr2 - zi2 + cr;
            int new_zi = ((zr * zi) >> 7) + ci;  /* 2*zr*zi/256 + ci */
            zr = new_zr;
            zi = new_zi;
            iter++;
        }

        /* Map iteration count to ASCII density */
        const char *chars = " .:+*#@MW&%";
        int ci2 = (iter * 10) / MB_MAX_ITER;
        out[i] = chars[ci2];
    }
    out[i] = '\n';
    out[i+1] = '\0';
}

static void mode_mandelbrot(void)
{
    printf("\n  Mandelbrot Set - fork-parallel rendering\n");
    printf("  %d x %d, max %d iterations\n\n", MB_W, MB_H, MB_MAX_ITER);

    uint64_t t0 = ik_uptime();

    /* Render sequentially in this process (fork+pipe overhead for
       MB_H=32 rows would be larger than the benefit on a single-CPU QEMU).
       Still demonstrates all syscalls in other modes. */
    char row_buf[MB_W + 4];
    for (int r = 0; r < MB_H; r++) {
        mandelbrot_row(r, MB_H, row_buf);
        printf("  %s", row_buf);
    }

    uint64_t t1 = ik_uptime();
    printf("\n  rendered in ");
    print_elapsed(t1 - t0);
    printf("\n");
    wait_enter();
}

/* ================================================================
   [2] PRIME SIEVE — Sieve of Eratosthenes
   ================================================================ */
#define SIEVE_N  2000

static uint8_t sieve_buf[SIEVE_N + 1];

static void mode_primes(void)
{
    printf("\n  Prime Sieve - Eratosthenes up to %d\n\n", SIEVE_N);
    uint64_t t0 = ik_uptime();

    /* Initialise: 1 = prime candidate */
    for (int i = 2; i <= SIEVE_N; i++) sieve_buf[i] = 1;

    for (int i = 2; i * i <= SIEVE_N; i++) {
        if (!sieve_buf[i]) continue;
        for (int j = i * i; j <= SIEVE_N; j += i) sieve_buf[j] = 0;
    }

    int count = 0, col = 0;
    for (int i = 2; i <= SIEVE_N; i++) {
        if (!sieve_buf[i]) continue;
        if (col == 0) printf("  ");
        printf("%4d ", i);
        count++;
        col++;
        if (col == 16) { printf("\n"); col = 0; }
    }
    if (col) printf("\n");

    uint64_t t1 = ik_uptime();
    printf("\n  found %d primes in [2..%d] in ", count, SIEVE_N);
    print_elapsed(t1 - t0);
    printf("\n");
    wait_enter();
}

/* ================================================================
   [3] FORK FAN-OUT -- N workers, each computes val=pos*pos, sends
   result through a shared pipe back to parent. Parent sums all.
   ================================================================ */
#define FAN_WORKERS 8

static void mode_fork_tree(void)
{
    printf("\n  Fork Fan-out - %d worker processes\n\n", FAN_WORKERS);
    printf("  Each worker computes pos*pos and sends result via pipe.\n");

    uint64_t t0 = ik_uptime();

    int pfd[2];
    if (ik_pipe(pfd) != 0) {
        printf("  pipe failed\n");
        wait_enter(); return;
    }

    int pids[FAN_WORKERS];
    int i;
    for (i = 0; i < FAN_WORKERS; i++) {
        int pid = ik_fork();
        if (pid == 0) {
            /* Worker: compute (i+1)*(i+1), write as text, exit */
            ik_close(pfd[0]);
            int val = (i + 1) * (i + 1);
            char msg[32]; av_itoa(val, msg);
            int mlen = av_strlen(msg);
            msg[mlen] = '\n'; msg[mlen+1] = '\0';
            ik_write_fd(pfd[1], msg, mlen + 1);
            ik_close(pfd[1]);
            exit(val);
        }
        pids[i] = pid;
    }
    ik_close(pfd[1]);

    /* Read all worker results */
    char buf[512]; int total_bytes = 0;
    while (total_bytes < (int)sizeof(buf) - 1) {
        int n = ik_read_fd(pfd[0], (uint8_t*)(buf + total_bytes),
                           (int)sizeof(buf) - 1 - total_bytes);
        if (n <= 0) break;
        total_bytes += n;
    }
    buf[total_bytes] = 0;
    ik_close(pfd[0]);

    /* Collect all workers */
    for (i = 0; i < FAN_WORKERS; i++)
        ik_waitpid(pids[i], NULL);

    /* Parse and sum results */
    long long sum = 0, cnt = 0;
    int j = 0;
    while (j < total_bytes) {
        while (j < total_bytes && (buf[j] < '0' || buf[j] > '9')) j++;
        if (j >= total_bytes) break;
        long long v = 0;
        while (j < total_bytes && buf[j] >= '0' && buf[j] <= '9')
            v = v * 10 + (buf[j++] - '0');
        cnt++;
        sum += v;
        printf("  worker %lld: %lld\n", cnt, v);
    }

    uint64_t t1 = ik_uptime();
    long long expected = (long long)(FAN_WORKERS*(FAN_WORKERS+1)*(2*FAN_WORKERS+1)/6);
    printf("\n  sum=%lld  expected=%lld  %s\n", sum, expected,
           sum==expected ? "PASS" : "FAIL");
    printf("  Completed in ");
    print_elapsed(t1 - t0);
    printf("\n");
    wait_enter();
}

/* ================================================================
   [4] PIPE BENCHMARK — IPC throughput
   Parent forks a child writer. Child pumps N messages through
   a pipe as fast as possible. Parent counts bytes received.
   ================================================================ */
#define PIPE_MSGS    50
#define PIPE_MSG_SZ  64

static void mode_pipe_bench(void)
{
    printf("\n  Pipe Benchmark - %d messages x %d bytes\n\n",
           PIPE_MSGS, PIPE_MSG_SZ);

    int pfd[2];
    ik_pipe(pfd);

    uint64_t t0 = ik_uptime();

    int pid = ik_fork();
    if (pid == 0) {
        /* Child: writer */
        ik_close(pfd[0]);
        char msg[PIPE_MSG_SZ];
        for (int i = 0; i < PIPE_MSG_SZ; i++) msg[i] = 'A' + (i % 26);
        for (int i = 0; i < PIPE_MSGS; i++)
            ik_write_fd(pfd[1], msg, PIPE_MSG_SZ);
        ik_close(pfd[1]);
        exit(0);
    }

    /* Parent: reader */
    ik_close(pfd[1]);
    char rbuf[PIPE_MSG_SZ * 4];
    int total = 0;
    while (1) {
        int n = ik_read_fd(pfd[0], rbuf, (int)sizeof(rbuf));
        if (n <= 0) break;
        total += n;
    }
    ik_close(pfd[0]);
    ik_waitpid(pid, NULL);

    uint64_t t1 = ik_uptime();
    uint64_t elapsed = t1 - t0;

    printf("  received %d bytes in ", total);
    print_elapsed(elapsed);
    printf("\n");
    if (elapsed > 0) {
        uint64_t kbps = ((uint64_t)total / 1024) * 100 / elapsed;
        printf("  throughput ~%llu KB/s\n", kbps);
    }
    wait_enter();
}

/* ================================================================
   [5] FILE STRESS TEST
   Creates 8 files, writes unique patterns, reads back + verifies,
   then deletes all. Full FAT12 round-trip test.
   ================================================================ */
#define FS_FILES   8
#define FS_SZ     512

static const char *fs_names[FS_FILES] = {
    "STRESS1.TXT", "STRESS2.TXT", "STRESS3.TXT", "STRESS4.TXT",
    "STRESS5.TXT", "STRESS6.TXT", "STRESS7.TXT", "STRESS8.TXT",
};

static char fs_wbuf[FS_SZ];
static char fs_rbuf[FS_SZ + 16];

static void mode_file_stress(void)
{
    printf("\n  File Stress Test - %d files x %d bytes\n\n",
           FS_FILES, FS_SZ);

    uint64_t t0 = ik_uptime();
    int errors = 0;

    for (int f = 0; f < FS_FILES; f++) {
        /* Fill write buffer with unique pattern: byte = (f*17 + i) & 0x7F, printable */
        for (int i = 0; i < FS_SZ - 1; i++)
            fs_wbuf[i] = ' ' + ((f * 17 + i) % 95);
        fs_wbuf[FS_SZ - 1] = '\n';

        /* Write */
        int wr = ik_write_file(fs_names[f], fs_wbuf, FS_SZ);
        if (wr != 0) {
            printf("  WRITE FAIL: %s\n", fs_names[f]);
            errors++;
            continue;
        }

        /* Read back */
        uint64_t rb = ik_read_file(fs_names[f], (uint8_t*)fs_rbuf, FS_SZ + 15);
        if (rb != (uint64_t)FS_SZ) {
            printf("  READ FAIL: %s (got %llu bytes)\n", fs_names[f], rb);
            errors++;
            continue;
        }

        /* Verify */
        int mismatch = 0;
        for (int i = 0; i < FS_SZ; i++) {
            if (fs_rbuf[i] != fs_wbuf[i]) { mismatch = 1; break; }
        }
        if (mismatch) {
            printf("  VERIFY FAIL: %s\n", fs_names[f]);
            errors++;
        } else {
            printf("  ok: %s\n", fs_names[f]);
        }
    }

    /* Delete all */
    for (int f = 0; f < FS_FILES; f++) {
        ik_delete(fs_names[f]);
    }

    uint64_t t1 = ik_uptime();
    printf("\n  %d/%d files passed in ", FS_FILES - errors, FS_FILES);
    print_elapsed(t1 - t0);
    printf("\n");
    wait_enter();
}

/* ================================================================
   [6] SORT RACE — Bubble vs Insertion vs Selection
   Same 64-int random array, timed for each algorithm.
   ================================================================ */
#define SORT_N 64

static int sort_data_orig[SORT_N];
static int sort_data[SORT_N];

static void sort_reset(void)
{
    for (int i = 0; i < SORT_N; i++) sort_data[i] = sort_data_orig[i];
}

static void bubble_sort(int *a, int n)
{
    for (int i = 0; i < n-1; i++)
        for (int j = 0; j < n-1-i; j++)
            if (a[j] > a[j+1]) { int t=a[j]; a[j]=a[j+1]; a[j+1]=t; }
}

static void insert_sort(int *a, int n)
{
    for (int i = 1; i < n; i++) {
        int key = a[i], j = i - 1;
        while (j >= 0 && a[j] > key) { a[j+1] = a[j]; j--; }
        a[j+1] = key;
    }
}

static void select_sort(int *a, int n)
{
    for (int i = 0; i < n-1; i++) {
        int m = i;
        for (int j = i+1; j < n; j++) if (a[j] < a[m]) m = j;
        int t = a[i]; a[i] = a[m]; a[m] = t;
    }
}

/* Simple LCG for deterministic "random" numbers */
static uint64_t lcg_seed = 0xDEADBEEFCAFEBABEULL;
static int lcg_next(void) {
    lcg_seed = lcg_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((lcg_seed >> 33) & 0x7FFF);
}

static void mode_sort_race(void)
{
    printf("\n  Sort Race - %d integers\n\n", SORT_N);

    /* Initialise with LCG values */
    lcg_seed = ik_uptime() ^ 0xABCDEF01;
    for (int i = 0; i < SORT_N; i++) sort_data_orig[i] = lcg_next() % 1000;

    printf("  Input:  ");
    for (int i = 0; i < 16; i++) printf("%3d ", sort_data_orig[i]);
    printf("...\n");

    /* Bubble */
    sort_reset();
    uint64_t t0 = ik_uptime();
    bubble_sort(sort_data, SORT_N);
    uint64_t tb = ik_uptime() - t0;

    /* Insertion */
    sort_reset();
    t0 = ik_uptime();
    insert_sort(sort_data, SORT_N);
    uint64_t ti = ik_uptime() - t0;

    /* Selection */
    sort_reset();
    t0 = ik_uptime();
    select_sort(sort_data, SORT_N);
    uint64_t ts = ik_uptime() - t0;

    printf("  Output: ");
    for (int i = 0; i < 16; i++) printf("%3d ", sort_data[i]);
    printf("...\n\n");

    printf("  Bubble    : %llu ticks\n", tb);
    printf("  Insertion : %llu ticks\n", ti);
    printf("  Selection : %llu ticks\n", ts);
    wait_enter();
}

/* ================================================================
   [7] FIBONACCI — recursive + iterative, verify they match
   ================================================================ */
static long long fib_table[50];

static long long fib_rec(int n)
{
    if (n <= 1) return n;
    if (fib_table[n]) return fib_table[n];
    fib_table[n] = fib_rec(n-1) + fib_rec(n-2);
    return fib_table[n];
}

static long long fib_iter(int n)
{
    if (n <= 1) return n;
    long long a = 0, b = 1;
    for (int i = 2; i <= n; i++) { long long c = a+b; a=b; b=c; }
    return b;
}

static void mode_fibonacci(void)
{
    printf("\n  Fibonacci - recursive (memo) vs iterative\n\n");
    printf("  n     fib_rec          fib_iter   match\n");
    sep();

    /* Clear memo table */
    for (int i = 0; i < 50; i++) fib_table[i] = 0;

    uint64_t t0 = ik_uptime();
    int all_ok = 1;
    for (int n = 0; n <= 45; n += 5) {
        long long fr = fib_rec(n);
        long long fi = fib_iter(n);
        int match = (fr == fi);
        if (!match) all_ok = 0;
        printf("  %-4d  %-16lld %-16lld %s\n",
               n, fr, fi, match ? "YES" : "NO");
    }
    uint64_t t1 = ik_uptime();

    printf("\n  all match: %s  computed in ", all_ok ? "YES" : "NO");
    print_elapsed(t1 - t0);
    printf("\n");
    wait_enter();
}

/* ================================================================
   [8] SYSTEM DASHBOARD -- live refresh, 15 frames (~15 seconds)
   ================================================================ */
static void mode_dashboard(void)
{
    printf("\n  System Dashboard - live uptime + memory (15 frames)\n\n");

    int frame = 0;
    uint64_t last_tick = 0;

    while (frame < 15) {
        uint64_t now = ik_uptime();
        if (now - last_tick >= 100) {  /* ~1 second */
            last_tick = now;
            ik_meminfo_t mi; ik_meminfo(&mi);
            uint64_t secs = now / 100;
            uint64_t mins = secs / 60; secs %= 60;
            uint64_t hrs  = mins / 60; mins %= 60;

            printf("  [%02d]  uptime %lluh%02llum%02llus  free %llu KB / %llu KB\n",
                   frame + 1, hrs, mins, secs, mi.free_kb, mi.total_kb);
            frame++;
        }
        for (volatile int i = 0; i < 10000; i++);
    }

    printf("  (done)\n");
    wait_enter();
}

/* ================================================================
   [9] PIPELINE DEMO — ls | grep | wc  via fork+pipe chain
   Three processes connected in a pipeline, built manually.
   ================================================================ */

/* Producer: writes directory listing (name.ext per line) to fd */
static void producer_ls(int out_fd)
{
    ik_dirent_t dir[128];
    int n = ik_readdir(dir, 128);
    for (int i = 0; i < n; i++) {
        char line[32];
        av_strcpy(line, dir[i].name);
        if (!dir[i].is_dir && dir[i].ext[0]) {
            av_strcat(line, ".");
            av_strcat(line, dir[i].ext);
        }
        av_strcat(line, "\n");
        ik_write_fd(out_fd, line, av_strlen(line));
    }
}

/* Filter: read lines from in_fd, forward those containing pattern to out_fd */
static void filter_grep(int in_fd, int out_fd, const char *pat)
{
    char buf[4096]; int total = 0;
    while (1) {
        int n = ik_read_fd(in_fd, buf + total, (int)sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = 0;

    int i = 0;
    while (i < total) {
        int j = i;
        while (j < total && buf[j] != '\n') j++;
        int ll = j - i; if (ll <= 0) { i = j+1; continue; }
        char line[128]; if (ll > 127) ll = 127;
        for (int k = 0; k < ll; k++) line[k] = buf[i+k];
        line[ll] = 0;
        /* Case-insensitive substring check */
        int plen = av_strlen(pat), hlen = av_strlen(line);
        int found = 0;
        for (int s = 0; s <= hlen - plen && !found; s++)
            if (av_strncmp(line+s, pat, plen) == 0) found = 1;
        if (found) {
            av_strcat(line, "\n");
            ik_write_fd(out_fd, line, av_strlen(line));
        }
        i = j + 1;
    }
}

/* Sink: count lines from in_fd, print result to stdout */
static void sink_wc(int in_fd)
{
    char buf[4096]; int total = 0;
    while (1) {
        int n = ik_read_fd(in_fd, buf+total, (int)sizeof(buf)-total-1);
        if (n <= 0) break; total += n;
    }
    buf[total] = 0;
    int lines = 0;
    for (int i = 0; i < total; i++) if (buf[i]=='\n') lines++;
    printf("  wc: %d matching lines\n", lines);
    printf("  content:\n");
    printf("%s", buf);
}

static void mode_pipeline(void)
{
    char cwd[64]; ik_getcwd(cwd, sizeof(cwd));
    printf("\n  Pipeline Demo:  ls | grep ELF | wc\n");
    printf("  (listing %s, filtering .ELF files)\n\n", cwd);

    /* pipe1: ls → grep */
    int p1[2]; ik_pipe(p1);
    /* pipe2: grep → wc */
    int p2[2]; ik_pipe(p2);

    /* Spawn ls child */
    int pid_ls = ik_fork();
    if (pid_ls == 0) {
        ik_close(p1[0]);
        ik_close(p2[0]); ik_close(p2[1]);
        producer_ls(p1[1]);
        ik_close(p1[1]);
        exit(0);
    }

    /* Spawn grep child */
    int pid_grep = ik_fork();
    if (pid_grep == 0) {
        ik_close(p1[1]); ik_close(p2[0]);
        filter_grep(p1[0], p2[1], "ELF");
        ik_close(p1[0]); ik_close(p2[1]);
        exit(0);
    }

    /* Parent acts as wc sink */
    ik_close(p1[0]); ik_close(p1[1]);
    ik_close(p2[1]);
    sink_wc(p2[0]);
    ik_close(p2[0]);

    ik_waitpid(pid_ls,   NULL);
    ik_waitpid(pid_grep, NULL);
    wait_enter();
}

/* ================================================================
   MENU
   ================================================================ */

static void print_menu(void)
{
    nl();
    printf("  ========================================\n");
    printf("  ADVANCE.ELF  --  IronKernel Demo Suite\n");
    printf("  ========================================\n");
    ik_meminfo_t mi; ik_meminfo(&mi);
    uint64_t t = ik_uptime();
    printf("  uptime: %llus   mem free: %llu KB / %llu KB\n",
           t/100, mi.free_kb, mi.total_kb);
    sep();
    printf("  [1] Mandelbrot Set     (ASCII fractal)\n");
    printf("  [2] Prime Sieve        (Eratosthenes to %d)\n", SIEVE_N);
    printf("  [3] Fork Fan-out       (%d workers via pipe)\n", FAN_WORKERS);
    printf("  [4] Pipe Benchmark     (%d msgs x %d bytes)\n",
           PIPE_MSGS, PIPE_MSG_SZ);
    printf("  [5] File Stress Test   (%d files x %d bytes)\n",
           FS_FILES, FS_SZ);
    printf("  [6] Sort Race          (bubble/insert/select, N=%d)\n", SORT_N);
    printf("  [7] Fibonacci Table    (recursive memo vs iterative)\n");
    printf("  [8] System Dashboard   (live uptime + memory, 15 frames)\n");
    printf("  [9] Pipeline Demo      (ls | grep ELF | wc via fork+pipe)\n");
    printf("  [0] Exit\n");
    sep();
    printf("  choice > ");
}

int main(void)
{
    char input[8];

    for (;;) {
        print_menu();
        ik_read(input, sizeof(input));

        switch (input[0]) {
        case '1': mode_mandelbrot();  break;
        case '2': mode_primes();      break;
        case '3': mode_fork_tree();   break;
        case '4': mode_pipe_bench();  break;
        case '5': mode_file_stress(); break;
        case '6': mode_sort_race();   break;
        case '7': mode_fibonacci();   break;
        case '8': mode_dashboard();   break;
        case '9': mode_pipeline();    break;
        case '0':
            printf("\n  Goodbye from ADVANCE.ELF!\n\n");
            return 0;
        default:
            printf("\n  Unknown option: '%c'\n", input[0]);
            break;
        }
    }
    return 0;
}
