#ifndef SCROLL_H
#define SCROLL_H

#include "types.h"

void scroll_init(void);
/* Initialise the scrollback ring buffer. Call once after vga_init. */

void scroll_on_newline(void);
/* Call every time the terminal scrolls up one row.
   Saves the departing top row into the ring buffer. */

void scroll_pgup(void);
/* Shift the visible window back 12 rows into history. */

void scroll_pgdn(void);
/* Shift the visible window forward — snap back to live screen. */

void scroll_reset(void);
/* Immediately restore the live screen. Call before any keypress write. */

#endif

int  scroll_is_active(void);
/* Returns 1 if the user has scrolled up (view_offset != 0). */
