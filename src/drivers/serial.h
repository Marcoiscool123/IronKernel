#ifndef SERIAL_H
#define SERIAL_H

/* IronKernel — serial.h
   COM1 debug output driver (115200 baud, 8N1, polled).
   All klog() messages and panic output are mirrored here.
   QEMU: add  -serial file:serial.log  (or -serial stdio) to the run target. */

void serial_init(void);
void serial_putchar(char c);
void serial_puts(const char *s);

#endif
