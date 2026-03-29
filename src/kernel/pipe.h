/* IronKernel — pipe.h
   Kernel pipe: fixed-size ring buffer with reference-counted ends.
   Both ends are tracked per-fd in the task's fd table (sched.h).
   PIPE_BUF must be a power of two (index wrap uses & mask). */
#ifndef PIPE_H
#define PIPE_H

#include "types.h"

#define PIPE_BUF   4096   /* bytes per pipe ring buffer (power of 2) */
#define MAX_PIPES  32     /* max simultaneous pipes */

typedef struct {
    uint8_t  buf[PIPE_BUF];
    uint32_t head;        /* next byte to read  */
    uint32_t tail;        /* next byte to write */
    int      read_open;   /* number of open read-end fds  */
    int      write_open;  /* number of open write-end fds */
    int      used;        /* 1 = slot is allocated        */
} pipe_t;

void pipe_init(void);

/* Allocate a new pipe; returns pipe id (0..MAX_PIPES-1) or -1. */
int  pipe_alloc(void);

/* Increment/decrement reference counts (used by fork and close). */
void pipe_ref_read (int id);
void pipe_ref_write(int id);
void pipe_close_read (int id);
void pipe_close_write(int id);

/* Non-blocking I/O — return bytes actually transferred. */
int  pipe_write(int id, const uint8_t *buf, int len);
int  pipe_read (int id,       uint8_t *buf, int len);

/* Predicates used by blocking loops in syscall.c. */
int  pipe_is_empty   (int id);
int  pipe_write_open (int id);
int  pipe_read_open  (int id);

#endif
