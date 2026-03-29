/* IronKernel iklibc — ironkernel.h
   IronKernel-specific API: syscall numbers and all 11 wrapper declarations.
   Include this for file I/O, uptime, meminfo, and direct syscall access. */
#ifndef IRONKERNEL_H
#define IRONKERNEL_H

#include <stdint.h>
#include <stddef.h>

/* ── Syscall numbers ──────────────────────────────────────────────── */
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
#define SYS_FORK       11
#define SYS_WAITPID    12
#define SYS_PIPE       13
#define SYS_DUP2       14
#define SYS_CLOSE      15
#define SYS_READ_FD    16
#define SYS_WRITE_FD   17
#define SYS_EXEC       18
#define SYS_READDIR    19

/* ── Memory info struct ───────────────────────────────────────────── */
typedef struct {
    uint64_t total_kb;
    uint64_t free_kb;
} ik_meminfo_t;

/* ── Syscall wrappers ─────────────────────────────────────────────── */

/* Print a null-terminated string to the VGA console. */
void     ik_write    (const char *s);

/* Terminate the process with a status code. */
__attribute__((noreturn))
void     ik_exit     (int status);

/* Fork: returns child pid to parent, 0 to child, -1 on error. */
int      ik_fork     (void);

/* Wait for child pid to exit; stores exit code in *status if non-NULL.
   Returns child pid on success, -1 on error. */
int      ik_waitpid  (int pid, int *status);

/* Create a pipe; pipefd[0]=read end, pipefd[1]=write end.
   Returns 0 on success, -1 on failure. */
int      ik_pipe     (int pipefd[2]);

/* Replace newfd with a duplicate of oldfd (like POSIX dup2).
   Returns newfd on success, -1 on failure. */
int      ik_dup2     (int oldfd, int newfd);

/* Close a file descriptor.  Returns 0 on success, -1 on failure. */
int      ik_close    (int fd);

/* Read up to len bytes from fd into buf; blocks until data or EOF.
   Returns bytes read (0 = EOF, -1 = error). */
int      ik_read_fd  (int fd, void *buf, int len);

/* Write len bytes from buf to fd; blocks if pipe buffer is full.
   Returns bytes written, or -1 on error. */
int      ik_write_fd (int fd, const void *buf, int len);

/* Read up to bufsz-1 bytes of keyboard input into buf.
   Returns the number of bytes read (not including null terminator). */
uint64_t ik_read     (char *buf, uint64_t bufsz);

/* Write data to a file on the FAT12 disk.
   path must be "NAME.EXT" format (8.3, uppercase or lowercase).
   Returns 0 on success, -1 on failure. */
int      ik_write_file(const char *path, const void *data, uint64_t len);

/* Create a directory in the current working directory.
   Returns 0 on success, -1 on failure (e.g. already exists). */
int      ik_mkdir    (const char *name);

/* Copy current working directory path into buf (max bufsz bytes).
   Returns number of bytes written. */
uint64_t ik_getcwd   (char *buf, uint64_t bufsz);

/* Change current working directory. ".." to go up.
   Returns 0 on success, -1 if directory not found. */
int      ik_chdir    (const char *name);

/* Read a file from the FAT12 disk into buf.
   Returns bytes read on success, (uint64_t)-1 on failure. */
uint64_t ik_read_file(const char *path, void *buf, uint64_t bufsz);

/* Delete a file from the FAT12 disk.
   Returns 0 on success, -1 on failure. */
int      ik_delete   (const char *path);

/* Return PIT tick count since boot (100 ticks = 1 second). */
uint64_t ik_uptime   (void);

/* Fill out with {total_kb, free_kb} physical memory statistics. */
void     ik_meminfo  (ik_meminfo_t *out);

/* Replace the current process image with a new ELF64 binary.
   On success, never returns (process image is replaced).
   On failure (file not found, bad ELF), returns -1. */
int      ik_exec     (const char *path);

/* ── Directory listing ────────────────────────────────────────────── */
typedef struct {
    char     name[9];    /* null-terminated 8-char name  */
    char     ext[4];     /* null-terminated 3-char ext   */
    uint8_t  is_dir;     /* 1 = directory, 0 = file      */
    uint32_t size;       /* file size in bytes            */
} ik_dirent_t;

/* Fill buf with up to max entries from the current working directory.
   Returns the number of entries stored (0 = empty). */
int      ik_readdir  (ik_dirent_t *buf, int max);

/* Copy the ELF startup argument into buf (max bufsz bytes).
   The argument is the token after the ELF name on the exec command,
   e.g. "README.TXT" from "exec EDIT.ELF README.TXT".
   Returns the number of bytes copied (not including null). */
uint64_t ik_get_arg  (char *buf, uint64_t bufsz);

/* Play a blocking tone on the PC speaker.
   freq = frequency in Hz (e.g. 440 = A4), ms = duration in milliseconds. */
void     ik_beep     (uint32_t freq, uint32_t ms);

#endif
