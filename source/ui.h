// The log every line of output goes into, and the scrolling window over it
// that the screens draw.
#pragma once
#include <switch.h>

#define RED    "\x1b[31;1m"
#define YELLOW "\x1b[33;1m"
#define GREEN  "\x1b[32;1m"
#define CYAN   "\x1b[36;1m"
#define RESET  "\x1b[0m"

// Rows the log gets on screen; the rest is the menu and hints.
#define ROWS 41

// Scroll offset in lines. Changed by the event loop, clamped by print_lines.
extern int scroll;

void out(const char *fmt, ...);       // appends to the log; '\n' ends a line
void err(const char *what, Result rc); // "  what: 2xxx-xxxx (0x...)" in red
void clear_lines(void);               // empties the log before a reread
int ui_line_count(void);
const char *ui_line(int i);           // i must be < ui_line_count()

void print_range(int total);          // "(x-y of z)" hint after a screen
// Prints lines [first, end) from the current scroll offset. Returns the
// total line count of the range.
int print_lines(int first, int end);
