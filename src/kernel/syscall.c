/* IronKernel v0.04 — syscall.c
   Dispatches int $0x80 calls from ring-3 user programs.
   Convention: RAX=number  RBX=arg1  RCX=arg2  RDX=arg3
   All pointers from user space are safe to dereference directly
   because the first 1GB is identity-mapped with U/S bit set.
*/
#include "syscall.h"
#include "idt.h"
#include "tss.h"
#include "types.h"
#include "pmm.h"
#include "sched.h"
#include "vmm.h"
#include "heap.h"
#include "pipe.h"
#include "scroll.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"
#include "../drivers/fat12.h"
#include "../drivers/fat32.h"
#include "../drivers/pit.h"
#include "../drivers/elf.h"
#include "../drivers/mouse.h"
#include "../drivers/vga.h"
#include "wm.h"
#include "panic.h"
#include "../drivers/speaker.h"
#include "../drivers/ac97.h"

/* Debug port 0xE9 */
static inline void sc_e9s(const char *s) {
    while (*s) { __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)*s)); s++; }
}

/* ── internal: parse "NAME.EXT" string into 8+3 space-padded fields ── */
static void parse_83(const char* path, char* name8, char* ext3)
{
    /* FAT12 stores names as exactly 8 uppercase space-padded bytes
       and extension as exactly 3 uppercase space-padded bytes.
       This helper converts a normal "FILE.TXT" string into that format
       so we can pass it directly to fat12_write_file / fat12_delete. */
    int i = 0;
    for (i = 0; i < 8; i++) name8[i] = ' ';
    for (i = 0; i < 3; i++) ext3[i]  = ' ';
    name8[8] = 0;
    ext3[3]  = 0;

    i = 0;
    while (path[i] && path[i] != '.' && i < 8) {
        char c = path[i];
        name8[i++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        /* Uppercase every character — FAT12 names are always uppercase. */
    }
    if (path[i] == '.') i++;
    int j = 0;
    while (path[i] && j < 3) {
        char c = path[i++];
        ext3[j++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
}

/* ── SYSCALL DISPATCH ───────────────────────────────────────────────── */
void syscall_dispatch(InterruptFrame *frame)
{
    switch (frame->rax) {

    /* ── SYS_WRITE (0) ──────────────────────────────────────────────
       Print a null-terminated string to the VGA console.
       RBX = virtual address of string in user space.
       Identity mapping means we can dereference it directly. */
    case SYS_WRITE: {
        const char *s = (const char*)(uintptr_t)frame->rbx;
        if (!s) break;
        fd_entry_t *fds = sched_get_tasks()[sched_current_id()].fds;
        if (fds[1].type == FD_PIPE_WRITE) {
            /* stdout → pipe: spin-write until all bytes fit. */
            int len = 0;
            while (s[len]) len++;
            int written = 0;
            __asm__ volatile("sti");
            while (written < len) {
                int n = pipe_write(fds[1].pipe_id,
                                   (const uint8_t*)s + written, len - written);
                written += n;
                if (n == 0) __asm__ volatile("pause");
            }
            __asm__ volatile("cli");
        } else {
            vga_print(s);
        }
        break;
    }

    /* ── SYS_EXIT (1) ───────────────────────────────────────────────
       RBX = exit status code.
       For directly exec'd processes: kernel_exit_restore() jumps back
       to elf_exec (via the saved kernel context in usermode.asm globals).
       For fork children (is_forked=1): mark TASK_ZOMBIE and yield so the
       parent's SYS_WAITPID can collect the exit code. */
    case SYS_EXIT: {
        int cur = sched_current_id();
        task_t *t = sched_get_tasks();
        t[cur].exit_code = (int)(int64_t)frame->rbx;
        if (t[cur].is_forked) {
            /* Close all pipe fds so write_open reaches 0 and readers wake. */
            for (int i = 0; i < TASK_MAX_FDS; i++) {
                if (t[cur].fds[i].type == FD_PIPE_READ)
                    pipe_close_read(t[cur].fds[i].pipe_id);
                else if (t[cur].fds[i].type == FD_PIPE_WRITE)
                    pipe_close_write(t[cur].fds[i].pipe_id);
                t[cur].fds[i].type = FD_NONE;
            }
            t[cur].state = TASK_ZOMBIE;
            __asm__ volatile("sti");
            sched_yield();
            while (1) __asm__ volatile("hlt"); /* never reached */
        }
        /* Restore kernel CR3 before kernel_exit_restore() jumps back to
           elf_exec: the saved kernel RSP points into the heap (task's
           kmalloc'd kernel stack) which is only correctly mapped under
           the kernel page table, not under the process's proc_cr3. */
        sc_e9s("[SYS_EXIT] before kernel_exit_restore\n");
        vmm_load_cr3(vmm_kernel_cr3());
        kernel_exit_restore();
        break; /* unreachable */
    }

    /* ── SYS_READ (2) ───────────────────────────────────────────────
       Read up to RCX bytes of keyboard input into buffer at RBX.
       Blocks (spins) until a newline or buffer full.
       Returns number of bytes read in RAX.
       This is a simple polling read — no blocking scheduler yet. */
    case SYS_READ: {
        char*    buf  = (char*)(uintptr_t)frame->rbx;
        uint64_t max  = frame->rcx;
        if (!buf || max == 0) { frame->rax = 0; break; }
        fd_entry_t *fds = sched_get_tasks()[sched_current_id()].fds;
        if (fds[0].type == FD_PIPE_READ) {
            /* stdin → pipe: block until data arrives or write end closes. */
            int pipe_id = fds[0].pipe_id;
            __asm__ volatile("sti");
            while (pipe_is_empty(pipe_id) && pipe_write_open(pipe_id))
                __asm__ volatile("pause");
            __asm__ volatile("cli");
            int n = pipe_read(pipe_id, (uint8_t*)buf, (int)(max - 1));
            buf[n] = 0;
            frame->rax = (uint64_t)n;
        } else {
            /* Default: keyboard. Re-enable interrupts so IRQ1 can fire. */
            uint64_t n = 0;
            __asm__ volatile("sti");
            while (n < max - 1) {
                while (!keyboard_haschar())
                    __asm__ volatile("pause");
                char c = keyboard_getchar();
                /* Scroll keys — same handling as kernel shell input loop. */
                if (c == KEY_UP)   { scroll_pgup(); continue; }
                if (c == KEY_DOWN) { scroll_pgdn(); continue; }
                /* Any real key snaps back to live view. */
                scroll_reset();
                if (c == '\n') { vga_print("\n"); break; }
                if (c == '\b') { if (n > 0) { n--; vga_print("\b"); } continue; }
                char s[2] = {c, 0};
                vga_print(s);
                buf[n++] = c;
            }
            __asm__ volatile("cli");
            buf[n] = 0;
            frame->rax = n;
        }
        break;
    }

    /* ── SYS_WRITE_FILE (3) ─────────────────────────────────────────
       Create or overwrite a file on the FAT12 disk.
       RBX = ptr to "NAME.EXT" filename string
       RCX = ptr to data buffer
       RDX = byte count
       Returns 0 in RAX on success, (uint64_t)-1 on failure. */
    case SYS_WRITE_FILE: {
        const char* path = (const char*)(uintptr_t)frame->rbx;
        const uint8_t* data = (const uint8_t*)(uintptr_t)frame->rcx;
        uint32_t len = (uint32_t)frame->rdx;
        char name8[9], ext3[4];
        parse_83(path, name8, ext3);
        int r = fat32_write_file(name8, ext3, data, len);
        frame->rax = (r == 0) ? 0 : (uint64_t)-1;
        break;
    }

    /* ── SYS_MKDIR (4) ──────────────────────────────────────────────
       Create a directory entry in the FAT12 root directory.
       RBX = ptr to directory name string (no extension).
       Returns 0 in RAX on success, (uint64_t)-1 on failure. */
    case SYS_MKDIR: {
        const char* dname = (const char*)(uintptr_t)frame->rbx;
        int r = fat32_mkdir(dname);
        frame->rax = (r == 0) ? 0 : (uint64_t)-1;
        break;
    }

    /* ── SYS_GETCWD (5) ─────────────────────────────────────────────
       Copy current working directory path into user buffer.
       RBX = destination buffer ptr
       RCX = buffer size in bytes
       Copies fat12_cwd (e.g. "/" or "/MYDIR") into user buffer. */
    case SYS_GETCWD: {
        char*    dst  = (char*)(uintptr_t)frame->rbx;
        uint64_t size = frame->rcx;
        if (!dst || size == 0) break;
        const char* src = fat32_cwd;
        uint64_t i = 0;
        while (i < size - 1 && src[i]) { dst[i] = src[i]; i++; }
        dst[i] = 0;
        frame->rax = i;
        break;
    }

    /* ── SYS_CHDIR (6) ──────────────────────────────────────────────
       Change current working directory.
       RBX = ptr to directory name string.
       Returns 0 in RAX on success, (uint64_t)-1 if not found. */
    case SYS_CHDIR: {
        const char* dname = (const char*)(uintptr_t)frame->rbx;
        int r = fat32_chdir(dname);
        frame->rax = (r == 0) ? 0 : (uint64_t)-1;
        break;
    }

    /* ── SYS_READ_FILE (7) ──────────────────────────────────────────
       Read a file from the FAT12 disk into a user buffer.
       RBX = ptr to "NAME.EXT\0" filename string
       RCX = ptr to destination buffer
       RDX = buffer size in bytes
       Returns bytes read in RAX, or (uint64_t)-1 on failure. */
    case SYS_READ_FILE: {
        const char* path = (const char*)(uintptr_t)frame->rbx;
        uint8_t*    buf  = (uint8_t*)(uintptr_t)frame->rcx;
        uint32_t    sz   = (uint32_t)frame->rdx;
        char name8[9], ext3[4];
        parse_83(path, name8, ext3);
        uint32_t bytes = 0;
        int r = fat32_read_file(name8, ext3, buf, sz, &bytes);
        frame->rax = (r == 0) ? (uint64_t)bytes : (uint64_t)-1;
        break;
    }

    /* ── SYS_DELETE (8) ─────────────────────────────────────────────
       Delete a file from the FAT12 disk.
       RBX = ptr to "NAME.EXT\0" filename string
       Returns 0 in RAX on success, (uint64_t)-1 on failure. */
    case SYS_DELETE: {
        const char* path = (const char*)(uintptr_t)frame->rbx;
        char name8[9], ext3[4];
        parse_83(path, name8, ext3);
        int r = fat32_delete(name8, ext3);
        frame->rax = (r == 0) ? 0 : (uint64_t)-1;
        break;
    }

    /* ── SYS_UPTIME (9) ─────────────────────────────────────────────
       Returns the number of PIT ticks since boot in RAX.
       100 ticks = 1 second (PIT configured at 100 Hz). */
    case SYS_UPTIME:
        frame->rax = (uint64_t)pit_get_ticks();
        break;

    /* ── SYS_MEMINFO (10) ───────────────────────────────────────────
       Writes total and free physical memory (in KB) to user buffer.
       RBX = ptr to uint64_t[2] — filled with {total_kb, free_kb}.
       Returns 0 in RAX. */
    case SYS_MEMINFO: {
        uint64_t* out = (uint64_t*)(uintptr_t)frame->rbx;
        if (out) {
            out[0] = pmm_get_total_frames() * 4;
            out[1] = pmm_get_free_frames()  * 4;
        }
        frame->rax = 0;
        break;
    }

    /* ── SYS_FORK (11) ──────────────────────────────────────────────
       Clone the calling process. Copies its user address space and
       creates a new scheduler task. The child resumes from the same
       point (after int $0x80) with RAX=0; parent gets child task id.
       Returns child pid in parent's RAX, or (uint64_t)-1 on error. */
    case SYS_FORK: {
        /* Switch to kernel CR3 for vmm_clone_user_pt frame zeroing,
           then restore the parent's user CR3 so it resumes correctly. */
        int cur_fork = sched_current_id();
        uint64_t parent_cr3 = sched_get_tasks()[cur_fork].cr3;
        vmm_load_cr3(vmm_kernel_cr3());
        int child_id = sched_fork(frame);
        vmm_load_cr3(parent_cr3);
        frame->rax = (child_id >= 0) ? (uint64_t)child_id : (uint64_t)-1;
        break;
    }

    /* ── SYS_WAITPID (12) ────────────────────────────────────────────
       RBX = child pid to wait for.
       RCX = ptr to int to receive exit status (or 0 to ignore).
       Temporarily unlocks the scheduler and enables interrupts so the
       child can run. Spins until child reaches TASK_ZOMBIE.
       Returns child pid in RAX, or (uint64_t)-1 on bad pid. */
    case SYS_WAITPID: {
        int     child_id = (int)(int64_t)frame->rbx;
        int    *status   = (int*)(uintptr_t)frame->rcx;
        task_t *ts       = sched_get_tasks();
        int     count    = sched_get_count();

        if (child_id < 1 || child_id >= count || !ts[child_id].is_forked) {
            frame->rax = (uint64_t)-1;
            break;
        }

        /* Unlock scheduler so child can be scheduled by PIT ticks. */
        sched_unlock();
        __asm__ volatile("sti");

        while (ts[child_id].state != TASK_ZOMBIE)
            __asm__ volatile("pause");

        __asm__ volatile("cli");
        /* sched_lock() intentionally omitted: cli already prevents the PIT
           from firing during cleanup, and leaving sched_ready=0 would
           permanently disable scheduling after this waitpid returns. */

        if (status) *status = ts[child_id].exit_code;

        /* Free child's user address space under kernel CR3 — the parent's
           user CR3 has holes in the identity map (split huge pages for user
           code) so accessing child page table frames as virtual addresses
           would fault. Kernel CR3 has the intact 1 GB identity map. */
        int cur = sched_current_id();
        uint64_t saved_cr3 = sched_get_tasks()[cur].cr3;
        vmm_load_cr3(vmm_kernel_cr3());
        vmm_free_user_pt(ts[child_id].cr3);
        vmm_load_cr3(saved_cr3);
        ts[child_id].cr3      = 0;
        kfree(ts[child_id].stack);   /* free kernel stack so slot can be recycled */
        ts[child_id].stack    = NULL;
        ts[child_id].kstack_top = 0;
        ts[child_id].is_forked  = 0;
        ts[child_id].state    = TASK_DEAD;

        frame->rax = (uint64_t)child_id;
        break;
    }

    /* ── SYS_PIPE (13) ──────────────────────────────────────────────
       Create a kernel pipe; write two fd numbers into the int[2] at RBX.
       fds_out[0] = read end, fds_out[1] = write end.
       Returns 0 in RAX on success, -1 on failure. */
    case SYS_PIPE: {
        int *fds_out = (int*)(uintptr_t)frame->rbx;
        if (!fds_out) { frame->rax = (uint64_t)-1; break; }

        int pipe_id = pipe_alloc();
        if (pipe_id < 0) { frame->rax = (uint64_t)-1; break; }

        int cur = sched_current_id();
        fd_entry_t *fds = sched_get_tasks()[cur].fds;

        /* Find two free slots starting from fd 3 (0=stdin, 1=stdout, 2=stderr). */
        int rfd = -1, wfd = -1;
        for (int i = 3; i < TASK_MAX_FDS; i++) {
            if (fds[i].type == FD_NONE) {
                if (rfd < 0)       { rfd = i; }
                else if (wfd < 0)  { wfd = i; break; }
            }
        }
        if (rfd < 0 || wfd < 0) {
            /* No two free slots — free the pipe and fail. */
            pipe_close_read(pipe_id);
            pipe_close_write(pipe_id);
            frame->rax = (uint64_t)-1;
            break;
        }

        fds[rfd].type    = FD_PIPE_READ;
        fds[rfd].pipe_id = pipe_id;
        fds[wfd].type    = FD_PIPE_WRITE;
        fds[wfd].pipe_id = pipe_id;

        fds_out[0] = rfd;
        fds_out[1] = wfd;
        frame->rax = 0;
        break;
    }

    /* ── SYS_DUP2 (14) ──────────────────────────────────────────────
       Replace newfd with a copy of oldfd (POSIX dup2 semantics).
       If newfd already refers to a pipe end the ref-count is decremented.
       RBX = oldfd, RCX = newfd → RAX = newfd or -1. */
    case SYS_DUP2: {
        int oldfd = (int)(int64_t)frame->rbx;
        int newfd = (int)(int64_t)frame->rcx;
        int cur   = sched_current_id();
        fd_entry_t *fds = sched_get_tasks()[cur].fds;

        if (oldfd < 0 || oldfd >= TASK_MAX_FDS ||
            newfd < 0 || newfd >= TASK_MAX_FDS) {
            frame->rax = (uint64_t)-1; break;
        }

        /* Close whatever newfd currently refers to. */
        if (fds[newfd].type == FD_PIPE_READ)
            pipe_close_read(fds[newfd].pipe_id);
        else if (fds[newfd].type == FD_PIPE_WRITE)
            pipe_close_write(fds[newfd].pipe_id);

        /* Copy and bump ref-count on the new end. */
        fds[newfd] = fds[oldfd];
        if (fds[newfd].type == FD_PIPE_READ)
            pipe_ref_read(fds[newfd].pipe_id);
        else if (fds[newfd].type == FD_PIPE_WRITE)
            pipe_ref_write(fds[newfd].pipe_id);

        frame->rax = (uint64_t)newfd;
        break;
    }

    /* ── SYS_CLOSE (15) ─────────────────────────────────────────────
       Close a file descriptor, decrementing any pipe ref-count.
       RBX = fd → RAX = 0 or -1. */
    case SYS_CLOSE: {
        int fd  = (int)(int64_t)frame->rbx;
        int cur = sched_current_id();
        fd_entry_t *fds = sched_get_tasks()[cur].fds;

        if (fd < 0 || fd >= TASK_MAX_FDS) {
            frame->rax = (uint64_t)-1; break;
        }
        if (fds[fd].type == FD_PIPE_READ)
            pipe_close_read(fds[fd].pipe_id);
        else if (fds[fd].type == FD_PIPE_WRITE)
            pipe_close_write(fds[fd].pipe_id);

        fds[fd].type    = FD_NONE;
        fds[fd].pipe_id = 0;
        frame->rax = 0;
        break;
    }

    /* ── SYS_READ_FD (16) ───────────────────────────────────────────
       Read up to RDX bytes from fd RBX into buffer RCX.
       Blocks until data is available or the write end is fully closed.
       Returns bytes read in RAX (0 = EOF, -1 = bad fd). */
    case SYS_READ_FD: {
        int      fd  = (int)(int64_t)frame->rbx;
        uint8_t *buf = (uint8_t*)(uintptr_t)frame->rcx;
        int      len = (int)frame->rdx;
        int      cur = sched_current_id();
        fd_entry_t *fds = sched_get_tasks()[cur].fds;

        if (fd < 0 || fd >= TASK_MAX_FDS || fds[fd].type != FD_PIPE_READ) {
            frame->rax = (uint64_t)-1; break;
        }
        int pipe_id = fds[fd].pipe_id;

        /* Block-read: collect bytes until EOF (write end closed + empty). */
        __asm__ volatile("sti");
        int total = 0;
        while (total < len) {
            while (pipe_is_empty(pipe_id) && pipe_write_open(pipe_id))
                __asm__ volatile("pause");
            if (pipe_is_empty(pipe_id))
                break;  /* EOF */
            int n = pipe_read(pipe_id, buf + total, len - total);
            total += n;
        }
        __asm__ volatile("cli");
        frame->rax = (uint64_t)total;
        break;
    }

    /* ── SYS_WRITE_FD (17) ──────────────────────────────────────────
       Write RDX bytes from buffer RCX to fd RBX.
       Spins if the pipe buffer is full.
       Returns bytes written in RAX (-1 = bad fd). */
    case SYS_WRITE_FD: {
        int            fd  = (int)(int64_t)frame->rbx;
        const uint8_t *buf = (const uint8_t*)(uintptr_t)frame->rcx;
        int            len = (int)frame->rdx;
        int            cur = sched_current_id();
        fd_entry_t    *fds = sched_get_tasks()[cur].fds;

        if (fd < 0 || fd >= TASK_MAX_FDS || fds[fd].type != FD_PIPE_WRITE) {
            frame->rax = (uint64_t)-1; break;
        }
        int pipe_id = fds[fd].pipe_id;
        int written = 0;
        __asm__ volatile("sti");
        while (written < len) {
            int n = pipe_write(pipe_id, buf + written, len - written);
            written += n;
            if (n == 0) __asm__ volatile("pause");
        }
        __asm__ volatile("cli");
        frame->rax = (uint64_t)written;
        break;
    }

    /* ── SYS_EXEC (18) ──────────────────────────────────────────────
       Replace the current process image with a new ELF64 binary.
       RBX = ptr to "PROG.ELF\0" filename in user space.
       Works by loading the ELF into a fresh page table, freeing the
       old user address space, then hijacking the interrupt frame so
       that iretq jumps directly to the new program's entry point.
       The task's is_forked flag is preserved — a forked child that
       calls SYS_EXEC will still become TASK_ZOMBIE on SYS_EXIT so
       the parent's SYS_WAITPID can collect its exit code.
       Returns (uint64_t)-1 in RAX on failure (load error).
       On success it never returns: the frame is redirected. */
    case SYS_EXEC: {
#define EXEC_STACK_TOP   0x701000ULL
#define EXEC_STACK_BASE  (EXEC_STACK_TOP - 4 * PAGE_SIZE)

        const char *path = (const char*)(uintptr_t)frame->rbx;
        if (!path) { frame->rax = (uint64_t)-1; break; }

        /* Copy path to local buffer while still under the caller's CR3 */
        char plocal[64];
        int pi = 0;
        while (path[pi] && pi < 63) { plocal[pi] = path[pi]; pi++; }
        plocal[pi] = 0;

        /* Save caller's CR3 so every failure path can restore it before
           returning to user mode — otherwise the iretq lands in ring 3
           with the kernel CR3 loaded and immediately page-faults. */
        uint64_t caller_cr3 = sched_get_tasks()[sched_current_id()].cr3;

        /* Switch to kernel CR3 before any frame allocation.
           The caller's user CR3 has split huge pages (pd[1]+ are non-huge),
           so pmm_alloc_frame() may return a frame at 0x200000+ that cannot
           be accessed as a virtual address under the user CR3.
           All kernel data (heap, BSS, stack) is in pd[0] — always reachable. */
        vmm_load_cr3(vmm_kernel_cr3());

        char name8[9], ext3[4];
        parse_83(plocal, name8, ext3);

        sched_lock();

        /* ── Load ELF file ── */
        uint8_t *ebuf = (uint8_t*)kmalloc(128 * 1024);
        if (!ebuf) { sched_unlock(); vmm_load_cr3(caller_cr3); frame->rax = (uint64_t)-1; break; }

        uint32_t ebytes = 0;
        if (fat32_read_file(name8, ext3, ebuf, 128 * 1024, &ebytes) != 0
            || ebytes < sizeof(Elf64Hdr)) {
            kfree(ebuf); sched_unlock(); vmm_load_cr3(caller_cr3); frame->rax = (uint64_t)-1; break;
        }

        Elf64Hdr *h = (Elf64Hdr*)ebuf;
        if (h->ident[0] != ELF_MAGIC0 || h->ident[1] != ELF_MAGIC1 ||
            h->ident[2] != ELF_MAGIC2 || h->ident[3] != ELF_MAGIC3 ||
            h->ident[4] != ELFCLASS64  || h->type != ET_EXEC ||
            h->machine  != EM_X86_64) {
            kfree(ebuf); sched_unlock(); vmm_load_cr3(caller_cr3); frame->rax = (uint64_t)-1; break;
        }

        /* ── Create new page table ── */
        uint64_t new_cr3 = vmm_create_user_pt();
        if (!new_cr3) {
            kfree(ebuf); sched_unlock(); vmm_load_cr3(caller_cr3); frame->rax = (uint64_t)-1; break;
        }

        /* ── Map PT_LOAD segments ── */
        int ok = 1;
        for (uint16_t si = 0; si < h->phnum && ok; si++) {
            Elf64Phdr *ph = (Elf64Phdr*)(ebuf + h->phoff +
                             (uint64_t)si * h->phentsize);
            if (ph->type != PT_LOAD || ph->memsz == 0) continue;

            uint64_t vstart = ph->vaddr & ~(PAGE_SIZE - 1);
            uint64_t vend   = (ph->vaddr + ph->memsz + PAGE_SIZE - 1)
                              & ~(PAGE_SIZE - 1);

            for (uint64_t va = vstart; va < vend && ok; va += PAGE_SIZE) {
                uint64_t fr = pmm_alloc_frame();
                if (!fr) { ok = 0; break; }
                uint8_t *fp = (uint8_t*)fr;
                for (uint32_t z = 0; z < PAGE_SIZE; z++) fp[z] = 0;

                uint64_t cs = (va > ph->vaddr) ? va : ph->vaddr;
                uint64_t ce_f = ph->vaddr + ph->filesz;
                uint64_t ce   = (va + PAGE_SIZE < ce_f) ? va + PAGE_SIZE : ce_f;
                if (cs < ce) {
                    uint8_t *src = ebuf + ph->offset + (cs - ph->vaddr);
                    uint8_t *dst = fp + (cs - va);
                    for (uint64_t b = 0; b < ce - cs; b++) dst[b] = src[b];
                }
                if (vmm_map_page_in(new_cr3, va, fr, PAGE_USER | PAGE_RW) != 0)
                    ok = 0;
            }
        }

        /* ── Map user stack ── */
        for (uint64_t va = EXEC_STACK_BASE; va < EXEC_STACK_TOP && ok; va += PAGE_SIZE) {
            uint64_t fr = pmm_alloc_frame();
            if (!fr) { ok = 0; break; }
            uint8_t *fp = (uint8_t*)fr;
            for (uint32_t z = 0; z < PAGE_SIZE; z++) fp[z] = 0;
            if (vmm_map_page_in(new_cr3, va, fr, PAGE_USER | PAGE_RW) != 0)
                ok = 0;
        }

        uint64_t entry = h->entry;
        kfree(ebuf);

        if (!ok) {
            vmm_load_cr3(vmm_kernel_cr3());
            vmm_free_user_pt(new_cr3);
            sched_unlock();
            vmm_load_cr3(caller_cr3);
            frame->rax = (uint64_t)-1;
            break;
        }

        /* ── Free old user address space (must run under kernel CR3) ── */
        int cur = sched_current_id();
        task_t *t = sched_get_tasks() + cur;
        uint64_t old_cr3 = t->cr3;

        vmm_load_cr3(vmm_kernel_cr3());
        vmm_free_user_pt(old_cr3);

        /* ── Switch to new address space ── */
        t->cr3 = new_cr3;
        vmm_load_cr3(new_cr3);
        tss_set_rsp0(t->irq_stack_top);  /* separate irq stack — must not overlap kstack */

        /* ── Hijack the interrupt frame — iretq will jump to new program ── */
        frame->rip    = entry;
        frame->rsp    = EXEC_STACK_TOP;
        frame->rflags = 0x202;   /* IF=1 */

        sched_unlock();
        frame->rax = 0;
        break;
#undef EXEC_STACK_TOP
#undef EXEC_STACK_BASE
    }

    /* ── SYS_READDIR (19) ────────────────────────────────────────────
       Fill a user buffer with directory entries from the current CWD.
       RBX = ptr to fat12_dentry_t array in user space
       RCX = max number of entries to fill
       Returns entry count in RAX (0 = empty directory). */
    case SYS_READDIR: {
        fat32_dentry_t *buf = (fat32_dentry_t*)(uintptr_t)frame->rbx;
        int max = (int)(int64_t)frame->rcx;
        if (!buf || max <= 0) { frame->rax = 0; break; }
        int n = fat32_readdir(fat32_cwd_cluster, buf, max);
        frame->rax = (uint64_t)n;
        break;
    }

    /* ── SYS_WIN_CREATE (20) ────────────────────────────────────────
       Create a new WM window for the calling task.
       RBX = ptr to title string, RCX = width, RDX = height
       Returns window id (>=0) in RAX, or (uint64_t)-1 on failure.
       The window is placed at a fixed offset so it doesn't overlap
       the GUI shell; the task's win_id is set so SYS_WRITE routes
       to this window automatically. */
    case SYS_WIN_CREATE: {
        const char *title = (const char*)(uintptr_t)frame->rbx;
        int w = (int)frame->rcx;
        int h = (int)frame->rdx;
        if (!title || w <= 0 || h <= 0) { frame->rax = (uint64_t)-1; break; }
        int id = wm_create(50, 50, w, h, title);
        if (id < 0) { frame->rax = (uint64_t)-1; break; }
        int cur = sched_current_id();
        sched_get_tasks()[cur].win_id = id;
        frame->rax = (uint64_t)id;
        break;
    }

    /* ── SYS_WIN_PRINT (21) ─────────────────────────────────────────
       Write a null-terminated string to the calling task's window.
       RBX = ptr to string.
       Uses the task's win_id (set by SYS_WIN_CREATE or wm_spawn_elf).
       Returns 0 in RAX, or (uint64_t)-1 if task has no window. */
    case SYS_WIN_PRINT: {
        const char *s = (const char*)(uintptr_t)frame->rbx;
        if (!s) { frame->rax = (uint64_t)-1; break; }
        int cur = sched_current_id();
        int win = sched_get_tasks()[cur].win_id;
        if (win < 0 || win >= WM_MAX_WIN || !wm_wins[win].alive) {
            frame->rax = (uint64_t)-1; break;
        }
        wm_puts(win, s);
        wm_wins[win].dirty = 1;
        frame->rax = 0;
        break;
    }

    /* ── SYS_WIN_PIXEL (22) ─────────────────────────────────────────
       Draw a single pixel inside the calling task's window client area.
       RBX = x (relative to window client), RCX = y, RDX = color (32bpp RGB).
       Returns 0 in RAX, or (uint64_t)-1 on error. */
    case SYS_WIN_PIXEL: {
        int x     = (int)(int64_t)frame->rbx;
        int y     = (int)(int64_t)frame->rcx;
        uint32_t color = (uint32_t)frame->rdx;
        int cur = sched_current_id();
        int win = sched_get_tasks()[cur].win_id;
        if (win < 0 || win >= WM_MAX_WIN || !wm_wins[win].alive) {
            frame->rax = (uint64_t)-1; break;
        }
        wm_win_t *wp = &wm_wins[win];
        /* Client area origin (skip 1px border + 18px title bar) */
        int px = wp->x + 1 + x;
        int py = wp->y + 1 + 18 + y;
        if (px >= 0 && px < SCR_W && py >= 0 && py < SCR_H)
            vga_pixel(px, py, color);
        wp->dirty = 1;
        frame->rax = 0;
        break;
    }

    /* ── SYS_WIN_CLOSE (23) ─────────────────────────────────────────
       Close the calling task's window and clear its win_id.
       Returns 0 in RAX. */
    case SYS_WIN_CLOSE: {
        int cur = sched_current_id();
        int win = sched_get_tasks()[cur].win_id;
        if (win >= 0 && win < WM_MAX_WIN) {
            wm_wins[win].alive = 0;
            sched_get_tasks()[cur].win_id = -1;
        }
        frame->rax = 0;
        break;
    }

    /* ── SYS_WIN_GFX_INIT (24) ──────────────────────────────────────
       Enter pixel-buffer GFX mode for the calling task's window.
       Computes client area dimensions, clears the buffer to dark background,
       and sets wm_elf_gfx_active = 1.
       Returns 0 in RAX on success, (uint64_t)-1 if no window. */
    case SYS_WIN_GFX_INIT: {
        int cur = sched_current_id();
        int win = sched_get_tasks()[cur].win_id;
        if (win < 0 || win >= WM_MAX_WIN || !wm_wins[win].alive) {
            /* No WM window: ELF called ik_gfx_init() from text mode.
               Print a helpful message and exit back to the text shell. */
            if (!vga_print_hook) {
                vga_print("\n  [ERROR] This ELF requires the window manager.\n");
                vga_print("  Type 'gui' to launch the desktop first.\n\n");
                vmm_load_cr3(vmm_kernel_cr3());
                kernel_exit_restore();
            }
            frame->rax = (uint64_t)-1; break;
        }
        wm_win_t *wp = &wm_wins[win];
        int cw = wp->w - 2;        /* 2 × BORDER (1px each side) */
        int ch = wp->h - 18 - 1;   /* TITLE_H=18, BORDER=1 */
        if (cw > WM_ELF_GFX_MAXW) cw = WM_ELF_GFX_MAXW;
        if (ch > WM_ELF_GFX_MAXH) ch = WM_ELF_GFX_MAXH;
        wm_elf_gfx_cw = cw;
        wm_elf_gfx_ch = ch;
        for (int i = 0; i < cw * ch; i++) wm_elf_gfx_buf[i] = 0x080C16u;
        wm_elf_gfx_active = 1;
        frame->rax = 0;
        break;
    }

    /* ── SYS_WIN_GFX_RECT (25) ──────────────────────────────────────
       Fill a rectangle in the ELF pixel buffer.
       RBX = (x<<16|y), RCX = (w<<16|h), RDX = color (32bpp). */
    case SYS_WIN_GFX_RECT: {
        if (!wm_elf_gfx_active) { frame->rax = (uint64_t)-1; break; }
        int x = (int)((frame->rbx >> 16) & 0xFFFF);
        int y = (int)(frame->rbx & 0xFFFF);
        int w = (int)((frame->rcx >> 16) & 0xFFFF);
        int h = (int)(frame->rcx & 0xFFFF);
        uint32_t color = (uint32_t)frame->rdx;
        vga_buf_rect(wm_elf_gfx_buf, wm_elf_gfx_cw, wm_elf_gfx_ch,
                     x, y, w, h, color);
        frame->rax = 0;
        break;
    }

    /* ── SYS_WIN_GFX_STR (26) ───────────────────────────────────────
       Draw a string into the ELF pixel buffer.
       RBX = (x<<16|y), RCX = ptr to null-terminated string, RDX = color. */
    case SYS_WIN_GFX_STR: {
        if (!wm_elf_gfx_active) { frame->rax = (uint64_t)-1; break; }
        int x = (int)((frame->rbx >> 16) & 0xFFFF);
        int y = (int)(frame->rbx & 0xFFFF);
        const char *s = (const char*)(uintptr_t)frame->rcx;
        uint32_t color = (uint32_t)frame->rdx;
        if (s) vga_buf_str(wm_elf_gfx_buf, wm_elf_gfx_cw, wm_elf_gfx_ch,
                           x, y, s, color);
        frame->rax = 0;
        break;
    }

    /* ── SYS_WIN_GFX_GRAD (27) ──────────────────────────────────────
       Draw a vertical gradient in the ELF pixel buffer.
       RBX = (x<<16|y), RCX = (w<<16|h), RDX = ptr to uint32_t[2]{c1,c2}. */
    case SYS_WIN_GFX_GRAD: {
        if (!wm_elf_gfx_active) { frame->rax = (uint64_t)-1; break; }
        int x = (int)((frame->rbx >> 16) & 0xFFFF);
        int y = (int)(frame->rbx & 0xFFFF);
        int w = (int)((frame->rcx >> 16) & 0xFFFF);
        int h = (int)(frame->rcx & 0xFFFF);
        const uint32_t *cols = (const uint32_t*)(uintptr_t)frame->rdx;
        if (!cols) { frame->rax = (uint64_t)-1; break; }
        vga_buf_gradient(wm_elf_gfx_buf, wm_elf_gfx_cw, wm_elf_gfx_ch,
                         x, y, w, h, cols[0], cols[1]);
        frame->rax = 0;
        break;
    }

    /* ── SYS_WIN_GFX_FLUSH (28) ─────────────────────────────────────
       Mark the calling task's window dirty so the WM redraws it.
       Returns 0 in RAX. */
    case SYS_WIN_GFX_FLUSH: {
        int cur = sched_current_id();
        int win = sched_get_tasks()[cur].win_id;
        if (win >= 0 && win < WM_MAX_WIN && wm_wins[win].alive)
            wm_wins[win].dirty = 1;
        frame->rax = 0;
        break;
    }

    /* ── SYS_GET_CLICK (29) ─────────────────────────────────────────
       Block until a left-click occurs inside the calling task's window
       client area.  Returns (x<<16)|y in RAX (client-relative coords),
       or (uint64_t)-1 if the task has no valid window. */
    case SYS_GET_CLICK: {
        int cur = sched_current_id();
        int win = sched_get_tasks()[cur].win_id;
        if (win < 0 || win >= WM_MAX_WIN || !wm_wins[win].alive) {
            frame->rax = (uint64_t)-1; break;
        }
        wm_win_t *wp = &wm_wins[win];
        int cx = wp->x + 1;        /* BORDER=1 */
        int cy = wp->y + 18 + 1;   /* TITLE_H=18, BORDER=1 */
        int cw = wp->w - 2;
        int ch = wp->h - 18 - 1;
        uint8_t prev_b = mouse_btn & 0x01u;
        __asm__ volatile("sti");
        while (1) {
            uint8_t cur_b = mouse_btn & 0x01u;
            if (cur_b && !prev_b) {
                int mx = mouse_x, my = mouse_y;
                int rx = mx - cx, ry = my - cy;
                if (rx >= 0 && ry >= 0 && rx < cw && ry < ch) {
                    __asm__ volatile("cli");
                    frame->rax = ((uint64_t)(uint32_t)rx << 16) |
                                  (uint64_t)(uint32_t)ry;
                    break;
                }
            }
            prev_b = cur_b;
            __asm__ volatile("pause");
        }
        break;
    }

    /* ── SYS_READ_RAW (30) ──────────────────────────────────────────
       Block until one raw keystroke is available; return it in RAX.
       No echo, no line buffering.  Special codes: KEY_UP=0x01,
       KEY_DOWN=0x02, KEY_LEFT=0x03, KEY_RIGHT=0x04,
       backspace=0x08, enter=0x0A.
       In text mode (win_id < 0) reads from keyboard directly without
       requiring window focus. */
    case SYS_READ_RAW: {
        int cur = sched_current_id();
        int win = sched_get_tasks()[cur].win_id;
        __asm__ volatile("sti");
        if (win >= 0 && win < WM_MAX_WIN) {
            /* GUI mode: wait for this window to be focused */
            while (wm_focused != win || !keyboard_haschar())
                __asm__ volatile("pause");
        } else {
            /* Text mode: read directly from keyboard */
            while (!keyboard_haschar())
                __asm__ volatile("pause");
        }
        char c = keyboard_getchar();
        __asm__ volatile("cli");
        frame->rax = (uint64_t)(uint8_t)c;
        break;
    }

    /* ── SYS_GET_ARG (31) ───────────────────────────────────────────
       Copy the ELF startup argument (set by wm_spawn_elf) into the
       user-supplied buffer.  The argument is the token that follows
       the ELF filename on the exec command line, e.g. "README.TXT"
       in  "exec EDIT.ELF README.TXT".
       RBX = destination buffer ptr
       RCX = buffer size in bytes
       Returns number of bytes copied (not including null) in RAX. */
    case SYS_GET_ARG: {
        char    *dst = (char*)(uintptr_t)frame->rbx;
        uint64_t sz  = frame->rcx;
        if (!dst || sz == 0) { frame->rax = 0; break; }
        const char *arg = sched_get_tasks()[sched_current_id()].elf_arg;
        uint64_t i = 0;
        while (i < sz - 1 && arg[i]) { dst[i] = arg[i]; i++; }
        dst[i] = 0;
        frame->rax = i;
        break;
    }

    /* ── SYS_TEST_PANIC (32) ────────────────────────────────────────
       Deliberately panic the kernel from ring-0 (syscall context).
       Used by ERROR.ELF to demonstrate a true kernel panic screen
       with ring-0 RIP, RSP, and stack trace.
       No args. Never returns. */
    case SYS_TEST_PANIC:
        panic("SYS_TEST_PANIC: kernel panic test triggered by user program");
        break; /* unreachable */

    /* ── SYS_BEEP (33) ──────────────────────────────────────────────
       Play a blocking tone on the PC speaker.
       RBX = frequency in Hz (0 = silence / turn off)
       RCX = duration in milliseconds */
    case SYS_BEEP: {
        uint32_t freq = (uint32_t)frame->rbx;
        uint32_t ms   = (uint32_t)frame->rcx;
        __asm__ volatile("sti");   /* int 0x80 clears IF; pit_get_ticks needs IRQs */
        speaker_beep(freq, ms);
        __asm__ volatile("cli");
        frame->rax = 0;
        break;
    }

    /* ── SYS_PLAY_WAV (34) ──────────────────────────────────────────
       Load a WAV file from FAT32 and play it via AC97 (blocking).
       RBX = pointer to "FILE.WAV\0" filename string.
       RAX ← 0 on success, -1 on error (no device / file not found / bad fmt). */
    case SYS_PLAY_WAV: {
        const char *path = (const char *)(uintptr_t)frame->rbx;
        if (!path) { frame->rax = (uint64_t)-1; break; }
        char name8[9], ext3[4];
        parse_83(path, name8, ext3);
        __asm__ volatile("sti");
        frame->rax = (uint64_t)ac97_play_file(name8, ext3);
        __asm__ volatile("cli");
        break;
    }

    default:
        frame->rax = (uint64_t)-1;
        break;
    }
}
