// Asks the person at the keyboard a yes-or-no question.
#ifndef ASK_TTY_H
#define ASK_TTY_H

// Fits the transport's `ask` slot. `ctx` is unused.
// Returns 1 for yes, 0 for no or an empty answer, and -1 when the program is not running in a
// terminal, so there is nobody to ask.
int ask_tty(void *ctx, const char *question);

#endif
