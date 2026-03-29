/* IronKernel — pipe.c
   Simple kernel pipe: ring buffer with two reference-counted ends.
   Readers spin (with interrupts enabled) until data arrives or the
   write end is fully closed (EOF).  Writers spin if the buffer is full. */
#include "pipe.h"

static pipe_t g_pipes[MAX_PIPES];

void pipe_init(void)
{
    for (int i = 0; i < MAX_PIPES; i++)
        g_pipes[i].used = 0;
}

int pipe_alloc(void)
{
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!g_pipes[i].used) {
            g_pipes[i].head       = 0;
            g_pipes[i].tail       = 0;
            g_pipes[i].read_open  = 1;
            g_pipes[i].write_open = 1;
            g_pipes[i].used       = 1;
            return i;
        }
    }
    return -1;
}

void pipe_ref_read (int id) { g_pipes[id].read_open++;  }
void pipe_ref_write(int id) { g_pipes[id].write_open++; }

void pipe_close_read(int id)
{
    pipe_t *p = &g_pipes[id];
    if (--p->read_open  <= 0 && p->write_open <= 0) p->used = 0;
}

void pipe_close_write(int id)
{
    pipe_t *p = &g_pipes[id];
    if (--p->write_open <= 0 && p->read_open  <= 0) p->used = 0;
}

int pipe_write(int id, const uint8_t *buf, int len)
{
    pipe_t  *p = &g_pipes[id];
    int n = 0;
    for (int i = 0; i < len; i++) {
        uint32_t next = (p->tail + 1) & (PIPE_BUF - 1);
        if (next == p->head) break;   /* buffer full */
        p->buf[p->tail] = buf[i];
        p->tail = next;
        n++;
    }
    return n;
}

int pipe_read(int id, uint8_t *buf, int len)
{
    pipe_t *p = &g_pipes[id];
    int n = 0;
    for (int i = 0; i < len; i++) {
        if (p->head == p->tail) break; /* buffer empty */
        buf[i] = p->buf[p->head];
        p->head = (p->head + 1) & (PIPE_BUF - 1);
        n++;
    }
    return n;
}

int pipe_is_empty   (int id) { return g_pipes[id].head == g_pipes[id].tail; }
int pipe_write_open (int id) { return g_pipes[id].write_open; }
int pipe_read_open  (int id) { return g_pipes[id].read_open;  }
