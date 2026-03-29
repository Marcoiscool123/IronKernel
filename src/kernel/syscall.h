/* IronKernel v0.04 — syscall.h */
#ifndef SYSCALL_H
#define SYSCALL_H
#include "idt.h"

/* ── SYSCALL TABLE ──────────────────────────────────────────────────
   Convention: RAX = number, RBX = arg1, RCX = arg2, RDX = arg3
   All syscalls triggered via  int $0x80  from ring-3.
   ─────────────────────────────────────────────────────────────── */

#define SYS_WRITE       0   /* RBX = ptr to null-terminated string          */
#define SYS_EXIT        1   /* no args — return to kernel/elf_exec          */
#define SYS_READ        2   /* RBX = buf ptr, RCX = max bytes               */
#define SYS_WRITE_FILE  3   /* RBX = "NAME.EXT\0", RCX = data, RDX = len   */
#define SYS_MKDIR       4   /* RBX = dirname ptr                            */
#define SYS_GETCWD      5   /* RBX = buf ptr, RCX = buf size                */
#define SYS_CHDIR       6   /* RBX = dirname ptr                            */
#define SYS_READ_FILE   7   /* RBX = "NAME.EXT\0", RCX = buf, RDX = bufsz  */
                            /* RAX ← bytes read, or (uint64_t)-1 on error  */
#define SYS_DELETE      8   /* RBX = "NAME.EXT\0" → RAX = 0 / -1           */
#define SYS_UPTIME      9   /* → RAX = PIT ticks since boot                 */
#define SYS_MEMINFO    10   /* RBX = ptr to uint64_t[2] {total_kb, free_kb} */
#define SYS_FORK       11   /* → RAX = child pid (parent) or 0 (child)      */
#define SYS_WAITPID    12   /* RBX = pid, RCX = ptr to int status           */
#define SYS_PIPE       13   /* RBX = int[2] out: [0]=read fd, [1]=write fd  */
#define SYS_DUP2       14   /* RBX = oldfd, RCX = newfd                     */
#define SYS_CLOSE      15   /* RBX = fd                                     */
#define SYS_READ_FD    16   /* RBX = fd, RCX = buf, RDX = len → RAX = n    */
#define SYS_WRITE_FD   17   /* RBX = fd, RCX = buf, RDX = len → RAX = n    */
#define SYS_EXEC       18   /* RBX = "PROG.ELF\0" → replace process image   */
#define SYS_READDIR    19   /* RBX = fat12_dentry_t* buf, RCX = max → count */
#define SYS_WIN_CREATE 20   /* RBX=title ptr, RCX=w, RDX=h → RAX=win_id    */
#define SYS_WIN_PRINT  21   /* RBX=string ptr → write to own window          */
#define SYS_WIN_PIXEL  22   /* RBX=x, RCX=y, RDX=color → pixel in win area  */
#define SYS_WIN_CLOSE  23   /* close own window                              */

/* ── GUI toolkit (ikgfx) syscalls ──────────────────────────────────── */
#define SYS_WIN_GFX_INIT  24  /* enter pixel-buffer mode for own window        */
#define SYS_WIN_GFX_RECT  25  /* RBX=(x<<16|y) RCX=(w<<16|h) RDX=color        */
#define SYS_WIN_GFX_STR   26  /* RBX=(x<<16|y) RCX=str_ptr   RDX=color        */
#define SYS_WIN_GFX_GRAD  27  /* RBX=(x<<16|y) RCX=(w<<16|h) RDX=uint32_t[2] */
#define SYS_WIN_GFX_FLUSH 28  /* mark window dirty → triggers WM redraw        */
#define SYS_GET_CLICK     29  /* block until click; RAX=(x<<16)|y in client    */
#define SYS_READ_RAW      30  /* block until keypress; RAX=raw char (no echo)  */
#define SYS_GET_ARG       31  /* RBX=buf, RCX=sz → copy elf_arg into buf       */
#define SYS_TEST_PANIC    32  /* deliberately panic the kernel from ring-0      */
#define SYS_BEEP          33  /* RBX=freq Hz, RCX=ms — blocking PC speaker beep */
#define SYS_PLAY_WAV      34  /* RBX="FILE.WAV\0" — load from FAT32, play; RAX=0/-1 */

void syscall_dispatch(InterruptFrame *frame);

#endif
