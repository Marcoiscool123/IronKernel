#include "scroll.h"
#include "../drivers/vga.h"

void scroll_init(void)       { vga_scroll_init(); }
void scroll_on_newline(void) { /* handled internally by vga.c */ }
void scroll_pgup(void)       { vga_view_up(); }
void scroll_pgdn(void)       { vga_view_down(); }
void scroll_reset(void)      { vga_view_reset(); }
int  scroll_is_active(void)  { return vga_scroll_active(); }
