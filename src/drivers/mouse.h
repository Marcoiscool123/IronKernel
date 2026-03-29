#ifndef MOUSE_H
#define MOUSE_H

#include "../kernel/types.h"

extern int     mouse_x, mouse_y;   /* current cursor position      */
extern uint8_t mouse_btn;          /* bit0=L bit1=R bit2=M         */
extern int     mouse_moved;        /* set to 1 each time state changes */

void mouse_init(void);
void mouse_poll(void);
void mouse_disable(void);

#endif
