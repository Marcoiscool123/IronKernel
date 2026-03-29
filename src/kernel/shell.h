#ifndef SHELL_H
#define SHELL_H

#include "types.h"

#define SHELL_INPUT_MAX  256
/* Maximum number of characters in a single command line.
   256 bytes covers any reasonable built-in command + arguments.
   Enforced in shell_run — excess characters are discarded. */

#define SHELL_VERSION   "0.04"
#define SHELL_NAME      "IRONKERNEL"
#define SHELL_ARCH      "x86_64"
#define SHELL_YEAR      "1980"
/* Version constants used by the fetch and version commands.
   Changing these here updates every command that references them. */

void shell_run(void);
void shell_dispatch(char *line); /* also called from WM shell window */
/* Main shell loop. Reads one line at a time from the keyboard,
   parses the first token as a command name, dispatches to the
   matching built-in handler, prints the prompt again.
   Never returns — runs until the machine is powered off. */

#endif
